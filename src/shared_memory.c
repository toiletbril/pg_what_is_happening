/*
 * This file is part of pg_what_is_happening.
 * Copyright (C) 2025 toilebril
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 * See top-level LICENSE file.
 */

#include "postgres.h"

#include "shared_memory.h"

#include <errno.h>
#include <signal.h>

#include "compatibility.h"
#include "gucs.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

/* Global shared memory state pointer. */
PwhSharedMemoryHeader *PWH_SHMEM = NULL;

Size PWH_SHMEM_SIZE = 0;
Size PWH_BACKEND_ENTRY_STRIDE = 0;

/* Previous shmem startup hook. */
shmem_startup_hook_type PREV_SHMEM_STARTUP_HOOK = NULL;

PWH_LWLOCK_TRANCHE_ID_DECL;

static bool check_if_process_exists(u32 pid);
static bool signal_process(u32 pid, int sig);

static u64
calc_backend_entry_stride(void)
{
	Size size = sizeof(PwhSharedMemoryBackendEntry);
	size = add_size(size, PWH_GUC_MAX_QUERY_TEXT_LEN);
	size = add_size(
		size, mul_size(PWH_GUC_MAX_NODES_PER_QUERY, sizeof(PwhNodeMetrics)));
	return size;
}

static u64
calc_shared_memory_size(void)
{
	Size size = 0;

	size = add_size(size, sizeof(PwhSharedMemoryHeader));
	size = add_size(size, mul_size(PWH_GUC_MAX_TRACKED_QUERIES,
								   calc_backend_entry_stride()));

	return size;
}

void *
pwh_get_shared_memory_ptr(void)
{
	bool  was_found;
	void *p = ShmemInitStruct("pg_what_is_happening", calc_shared_memory_size(),
							  &was_found);
	Assert(was_found);
	return p;
}

void
pwh_shared_memory_startup_hook(void)
{
	if (PREV_SHMEM_STARTUP_HOOK)
		PREV_SHMEM_STARTUP_HOOK();

	PWH_SHMEM_REQUEST_IN_STARTUP_HOOK();

	PWH_SHMEM_SIZE = calc_shared_memory_size();
	PWH_BACKEND_ENTRY_STRIDE = calc_backend_entry_stride();

	bool was_found;
	PWH_SHMEM = ShmemInitStruct("pg_what_is_happening",
								calc_shared_memory_size(), &was_found);
	Assert(PWH_SHMEM != NULL);

	if (unlikely(!was_found))
	{
		ereport(LOG, (errmsg("PWH: Initializing shared memory"),
					  errdetail("%zu bytes for %d backend entries",
								calc_shared_memory_size(),
								PWH_GUC_MAX_TRACKED_QUERIES)));

		/* No lock -- we're the first to access this memory. */

		for (u64 i = 0; i < (u64) PWH_GUC_MAX_TRACKED_QUERIES; i++)
		{
			PwhSharedMemoryBackendEntry *be = PWH_GET_BACKEND_ENTRY_UNSAFE(i);
			Assert(be != NULL);

			/*
			 * First and last usage of memset() for backend entry.
			 * The value of 'poll_generation' field should be preserved.
			 */
			MemSet(be, 0, calc_backend_entry_stride());

			be->backend_pid = 0;
		}

		PWH_LWLOCK_SETUP_TRANCHE(PWH_LWLOCK_TRANCHE_ID, "pg_what_is_happening");
		PWH_LWLOCK_INITIALIZE(PWH_SHMEM->entry_search_lock,
							  PWH_LWLOCK_TRANCHE_ID);
	}
}

PwhSharedMemoryBackendEntry *
pwh_get_or_create_my_backend_entry_impl(bool should_create)
{
	PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_SHARED);

	u64 free_slot_idx = -1U;

	for (u64 i = 0; i < (u64) PWH_GUC_MAX_TRACKED_QUERIES; i++)
	{
		PwhSharedMemoryBackendEntry *be = PWH_GET_BACKEND_ENTRY_UNSAFE(i);
		Assert(be);

		if (free_slot_idx == -1U && be->backend_pid == 0)
		{
			free_slot_idx = i;
		}
		else if (be->backend_pid == MyProcPid)
		{
			PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);
			return be;
		}
	}

	if (should_create && free_slot_idx != -1U)
	{
		PwhSharedMemoryBackendEntry *be =
			PWH_GET_BACKEND_ENTRY_UNSAFE(free_slot_idx);

		ereport(DEBUG2, (errmsg("PWH: Allocated backend entry %lu for PID %u",
								free_slot_idx, MyProcPid)));
		be->backend_pid = MyProcPid;

		PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);
		return be;
	}

	PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);

	if (should_create)
	{
		ereport(LOG,
				(errmsg("PWH: All backend entries exhausted"),
				 errdetail("All %d slots are in use, PID %d cannot be tracked",
						   PWH_GUC_MAX_TRACKED_QUERIES, MyProcPid)));
	}

	return NULL;
}

/*
 * Get backend entry by index.
 */
PwhSharedMemoryBackendEntry *
pwh_get_backend_entry(u64 index)
{
	if (index >= (u64) PWH_GUC_MAX_TRACKED_QUERIES)
		return NULL;

	PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_SHARED);
	PwhSharedMemoryBackendEntry *be = PWH_GET_BACKEND_ENTRY_UNSAFE(index);
	PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);

	Assert(be != NULL);

	return be;
}

void
pwh_release_my_backend_entry(void)
{
	PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_SHARED);

	for (u64 i = 0; i < (u64) PWH_GUC_MAX_TRACKED_QUERIES; i++)
	{
		PwhSharedMemoryBackendEntry *be = PWH_GET_BACKEND_ENTRY_UNSAFE(i);
		Assert(be);

		if (be->backend_pid == MyProcPid)
		{
			pwh_release_backend_entry_unlocked(be);
			break;
		}
	}

	PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);
}

/*
 * Get pointer to query_text for a backend entry.
 */
char *
pwh_get_backend_entry_query_text(PwhSharedMemoryBackendEntry *entry)
{
	return (char *) entry + sizeof(PwhSharedMemoryBackendEntry);
}

/*
 * Get pointer to metrics array for a backend entry.
 */
PwhNodeMetrics *
pwh_get_backend_entry_metrics(PwhSharedMemoryBackendEntry *entry)
{
	return (PwhNodeMetrics *) ((char *) entry +
							   sizeof(PwhSharedMemoryBackendEntry) +
							   PWH_GUC_MAX_QUERY_TEXT_LEN);
}

/*
 * Send SIGUSR2 to all active backends to refresh metrics.
 * Returns the number of backends signaled.
 */
u32
pwh_request_backend_metrics_unlocked(void)
{
	u32 n_signaled = 0;

	for (u64 i = 0; i < (u64) PWH_GUC_MAX_TRACKED_QUERIES; i++)
	{
		PwhSharedMemoryBackendEntry *be = PWH_GET_BACKEND_ENTRY_UNSAFE(i);

		if (pwh_is_backend_entry_active(be))
		{
			if (signal_process(be->backend_pid, SIGUSR2))
				n_signaled++;
		}
	}

	return n_signaled;
}

bool
pwh_validate_node_magic(PwhNodeMetrics *node, u32 node_id)
{
	if (node->magic != PWH_NODE_MAGIC)
	{
		ereport(LOG, (errmsg("PWH: Node ID %u magic mismatch", node_id)));
		return false;
	}

	return true;
}

u32
pwh_cleanup_orphaned_slots(void)
{
	u32 n_cleaned = 0;

	for (u64 i = 0; i < (u64) PWH_GUC_MAX_TRACKED_QUERIES; i++)
	{
		PwhSharedMemoryBackendEntry *be = pwh_get_backend_entry(i);

		if (pwh_is_backend_entry_active(be))
		{
			if (!check_if_process_exists(be->backend_pid))
			{
				ereport(DEBUG1,
						(errmsg("PWH: Cleaning up orphaned slot %lu", i),
						 errdetail("Backend PID %u no longer exists",
								   be->backend_pid)));

				/* No one should be reading that memory anyway. */
				pwh_release_backend_entry_unlocked(be);

				n_cleaned++;
			}
		}
	}

	return n_cleaned;
}

static bool
check_if_process_exists(u32 pid)
{
	return kill((pid_t) pid, 0) == 0 || errno != ESRCH;
}

static bool
signal_process(u32 pid, int sig)
{
	return kill((pid_t) pid, sig) == 0;
}
