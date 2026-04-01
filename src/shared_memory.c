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
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

/* Global shared memory state pointer. */
PwhSharedMemoryHeader *PWH_SHMEM = NULL;

/* Previous shmem startup hook. */
shmem_startup_hook_type PREV_SHMEM_STARTUP_HOOK = NULL;

PWH_LWLOCK_TRANCHE_ID_DECL;

static void backend_exit_callback(int code, Datum arg);
static bool check_if_process_exists(u32 pid);
static bool signal_process(u32 pid, int sig);

Size
pwh_get_backend_entry_stride(void)
{
	Size size = sizeof(PwhSharedMemoryBackendEntry);
	size = add_size(size, PWH_QUERY_TEXT_LEN_GUC);
	size = add_size(
		size, mul_size(PWH_MAX_NODES_PER_QUERY_GUC, sizeof(PwhNodeMetrics)));
	return size;
}

Size
pwh_get_shared_memory_size(void)
{
	Size size = 0;

	size = add_size(size, sizeof(PwhSharedMemoryHeader));
	size = add_size(size, mul_size(PWH_MAX_TRACKED_QUERIES_GUC,
								   pwh_get_backend_entry_stride()));

	return size;
}

void *
pwh_get_shared_memory_ptr(void)
{
	bool  was_found;
	void *p = ShmemInitStruct("pg_what_is_happening",
							  pwh_get_shared_memory_size(), &was_found);
	Assert(was_found);
	return p;
}

void
pwh_shared_memory_startup_hook(void)
{
	if (PREV_SHMEM_STARTUP_HOOK)
		PREV_SHMEM_STARTUP_HOOK();

	PWH_SHMEM_REQUEST_IN_STARTUP_HOOK();

	bool was_found;
	PWH_SHMEM = ShmemInitStruct("pg_what_is_happening",
								pwh_get_shared_memory_size(), &was_found);
	Assert(PWH_SHMEM != NULL);

	if (unlikely(!was_found))
	{
		ereport(LOG, (errmsg("PWH: Initializing shared memory"),
					  errdetail("%zu bytes for %d backend entries",
								pwh_get_shared_memory_size(),
								PWH_MAX_TRACKED_QUERIES_GUC)));

		for (u64 i = 0; i < (u64) PWH_MAX_TRACKED_QUERIES_GUC; i++)
		{
			PwhSharedMemoryBackendEntry *shmem_be_entry =
				PWH_GET_BACKEND_ENTRY_UNSAFE(i);
			MemSet(shmem_be_entry, 0, pwh_get_backend_entry_stride());
			shmem_be_entry->backend_pid = 0;
			SpinLockInit(&shmem_be_entry->slot_lock);
			shmem_be_entry->query_text_capacity = (u32) PWH_QUERY_TEXT_LEN_GUC;
			shmem_be_entry->metrics_capacity =
				(u32) PWH_MAX_NODES_PER_QUERY_GUC;
		}

		PWH_LWLOCK_SETUP_TRANCHE(PWH_LWLOCK_TRANCHE_ID, "pg_what_is_happening");
		PWH_LWLOCK_INITIALIZE(PWH_SHMEM->lock, PWH_LWLOCK_TRANCHE_ID);
	}
}

PwhSharedMemoryBackendEntry *
pwh_get_my_backend_entry(void)
{
	for (u64 i = 0; i < (u64) PWH_MAX_TRACKED_QUERIES_GUC; i++)
	{
		PwhSharedMemoryBackendEntry *be = pwh_get_backend_entry(i);

		if (be->backend_pid == MyProcPid)
		{
			return be;
		}
	}

	PWH_LWLOCK_ACQUIRE(PWH_SHMEM->lock, LW_EXCLUSIVE);
	for (u64 i = 0; i < (u64) PWH_MAX_TRACKED_QUERIES_GUC; i++)
	{
		PwhSharedMemoryBackendEntry *be = pwh_get_backend_entry(i);

		if (be->backend_pid == 0)
		{
			be->backend_pid = MyProcPid;
			PWH_LWLOCK_RELEASE(PWH_SHMEM->lock);
			ereport(DEBUG2,
					(errmsg("PWH: Allocated backend entry %lu for PID %u", i,
							MyProcPid)));

			before_shmem_exit(backend_exit_callback, (Datum) 0);

			return be;
		}
	}
	PWH_LWLOCK_RELEASE(PWH_SHMEM->lock);

	ereport(DEBUG1,
			(errmsg("PWH: All backend entries exhausted"),
			 errdetail("All %d slots are in use, PID %d cannot be tracked",
					   PWH_MAX_TRACKED_QUERIES_GUC, MyProcPid)));

	return NULL;
}

/*
 * Get total number of backend entries.
 */
u64
pwh_get_backend_entry_count(void)
{
	return (u64) PWH_MAX_TRACKED_QUERIES_GUC;
}

/*
 * Get backend entry by index.
 */
PwhSharedMemoryBackendEntry *
pwh_get_backend_entry(u64 index)
{
	if (index >= (u64) PWH_MAX_TRACKED_QUERIES_GUC)
		return NULL;

	return PWH_GET_BACKEND_ENTRY_UNSAFE(index);
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
							   PWH_QUERY_TEXT_LEN_GUC);
}

/*
 * Send SIGUSR2 to all active backends to refresh metrics.
 * Returns the number of backends signaled.
 */
u32
pwh_request_backend_metrics(void)
{
	u32 n_signaled = 0;

	for (u64 i = 0; i < (u64) PWH_MAX_TRACKED_QUERIES_GUC; i++)
	{
		PwhSharedMemoryBackendEntry *shmem_be_entry = pwh_get_backend_entry(i);

		if (shmem_be_entry != NULL && shmem_be_entry->backend_pid != 0)
		{
			if (signal_process(shmem_be_entry->backend_pid, SIGUSR2))
			{
				n_signaled++;
			}
		}
	}

	return n_signaled;
}

bool
pwh_validate_node_magic(PwhNodeMetrics *node, u32 node_id)
{
	unused(node_id);

	if (node->magic != PWH_NODE_MAGIC)
	{
		return false;
	}
	return true;
}

u32
pwh_cleanup_orphaned_slots(void)
{
	u32 n_cleaned = 0;

	for (u64 i = 0; i < (u64) PWH_MAX_TRACKED_QUERIES_GUC; i++)
	{
		PwhSharedMemoryBackendEntry *be = pwh_get_backend_entry(i);

		if (be != NULL && be->backend_pid != 0)
		{
			if (!check_if_process_exists(be->backend_pid))
			{
				ereport(DEBUG1,
						(errmsg("PWH: Cleaning up orphaned slot %lu", i),
						 errdetail("Backend PID %u no longer exists",
								   be->backend_pid)));

				be->backend_pid = 0;
				n_cleaned++;
			}
		}
	}

	return n_cleaned;
}

static bool
check_if_process_exists(u32 pid)
{
	return kill(pid, 0) == 0 || errno != ESRCH;
}

static bool
signal_process(u32 pid, int sig)
{
	return kill(pid, sig) == 0;
}

static void
backend_exit_callback(int code, Datum arg)
{
	unused(code);
	unused(arg);

	for (u64 i = 0; i < (u64) PWH_MAX_TRACKED_QUERIES_GUC; i++)
	{
		PwhSharedMemoryBackendEntry *be = pwh_get_backend_entry(i);

		if (be != NULL && be->backend_pid == MyProcPid)
		{
			be->backend_pid = 0;
			ereport(DEBUG2,
					(errmsg("PWH: Released backend entry %lu for PID %u", i,
							MyProcPid)));
			return;
		}
	}
}
