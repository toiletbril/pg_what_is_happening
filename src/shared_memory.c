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

/*
 * Shared memory-related routines. This is extensions' main way of interacting
 * with all currently-running backends on the host, either via BG worker or SQL
 * query.
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
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

/* XXX get rid of shared memory and use a socket instead. */

/* Global shared memory state pointer. */
PwhSharedMemoryHeader *PWH_SHMEM = NULL;

Size PWH_SHMEM_SIZE = 0;
Size PWH_BACKEND_ENTRY_STRIDE = 0;

/* Previous shmem startup hook. */
shmem_startup_hook_type PREV_SHMEM_STARTUP_HOOK = NULL;

PWH_LWLOCK_TRANCHE_ID_DECL;

static bool is_postgres_backend(u32 pid);
static bool timestamp_is_fresh(TimestampTz now, TimestampTz then,
							   TimestampTz freshness_us);
static bool signal_process(u32 pid, int sig);

static Size
calc_backend_entry_stride(void)
{
	Size size = sizeof(PwhSharedMemoryBackendEntry);
	size = add_size(size, PWH_GUC_MAX_QUERY_TEXT_LEN);
	size = MAXALIGN(size);
	size = add_size(
		size, mul_size(PWH_GUC_MAX_NODES_PER_QUERY, sizeof(PwhNodeMetrics)));
	return MAXALIGN(size);
}

static Size
calc_shared_memory_size(void)
{
	Size size = 0;

	size = add_size(size, MAXALIGN(sizeof(PwhSharedMemoryHeader)));
	size = add_size(size, mul_size(PWH_GUC_MAX_TRACKED_QUERIES,
								   calc_backend_entry_stride()));

	return size;
}

void
pwh_calculate_shared_memory_layout(void)
{
	PWH_BACKEND_ENTRY_STRIDE = calc_backend_entry_stride();
	PWH_SHMEM_SIZE = calc_shared_memory_size();
}

void *
pwh_get_shared_memory_ptr(void)
{
	bool  was_found;
	void *p =
		ShmemInitStruct("pg_what_is_happening", PWH_SHMEM_SIZE, &was_found);
	if (p == NULL || !was_found)
		ereport(
			ERROR,
			(errmsg("PWH: Shared memory is not initialized"),
			 errhint(
				 "Add pg_what_is_happening to shared_preload_libraries and restart PostgreSQL.")));
	return p;
}

void
pwh_shared_memory_startup_hook(void)
{
	if (PREV_SHMEM_STARTUP_HOOK)
		PREV_SHMEM_STARTUP_HOOK();

	bool was_found;
	PWH_SHMEM =
		ShmemInitStruct("pg_what_is_happening", PWH_SHMEM_SIZE, &was_found);
	Assert(PWH_SHMEM != NULL);

	if (unlikely(!was_found))
	{
		ereport(LOG, (errmsg("PWH: Initializing shared memory"),
					  errdetail("%zu bytes for %d backend entries",
								PWH_SHMEM_SIZE, PWH_GUC_MAX_TRACKED_QUERIES)));

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
		PWH_SHMEM->refresh_in_progress = false;
		PWH_SHMEM->refresh_owner_pid = 0;
		PWH_SHMEM->refresh_token = 0;
		PWH_SHMEM->refresh_generation = 0;
		PWH_SHMEM->last_refresh_time = 0;
	}
}

PwhSharedMemoryBackendEntry *
pwh_get_or_create_my_backend_entry_impl(bool should_create,
										bool should_acquire_lock)
{
	if (should_acquire_lock)
		PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock,
						   should_create ? LW_EXCLUSIVE : LW_SHARED);

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
			if (should_acquire_lock)
				PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);

			return be;
		}
	}

	if (should_create && free_slot_idx != -1U)
	{
		PwhSharedMemoryBackendEntry *be =
			PWH_GET_BACKEND_ENTRY_UNSAFE(free_slot_idx);

		ereport(DEBUG2,
				(errmsg("PWH: Allocated backend entry %llu for PID %u",
						(unsigned long long) free_slot_idx, MyProcPid)));
		be->owner_oid = GetUserId();
		be->query_id = 0;
		be->query_start_time = 0;
		be->count_of_metrics = 0;
		be->write_sequence = 0;
		pwh_get_backend_entry_query_text(be)[0] = '\0';
		PWH_MEMORY_BARRIER();
		be->backend_pid = MyProcPid;

		if (should_acquire_lock)
			PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);

		return be;
	}

	if (should_acquire_lock)
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
	PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_EXCLUSIVE);

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
							   MAXALIGN(sizeof(PwhSharedMemoryBackendEntry) +
										PWH_GUC_MAX_QUERY_TEXT_LEN));
}

bool
pwh_validate_node_magic(PwhNodeMetrics *node, u32 node_id)
{
	if (node->magic != PWH_NODE_MAGIC)
	{
		ereport(DEBUG1, (errmsg("PWH: Node ID %u magic mismatch", node_id)));
		return false;
	}

	return true;
}

u32
pwh_cleanup_orphaned_slots(void)
{
	u32	 n_cleaned = 0;
	u32	 count = 0;
	u32 *pids = palloc(sizeof(u32) * PWH_GUC_MAX_TRACKED_QUERIES);
	u32 *slot_indexes = palloc(sizeof(u32) * PWH_GUC_MAX_TRACKED_QUERIES);

	PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_SHARED);

	for (u64 i = 0; i < (u64) PWH_GUC_MAX_TRACKED_QUERIES; i++)
	{
		PwhSharedMemoryBackendEntry *be = PWH_GET_BACKEND_ENTRY_UNSAFE(i);
		if (pwh_is_backend_entry_active(be))
		{
			pids[count] = (u32) be->backend_pid;
			slot_indexes[count] = (u32) i;
			count++;
		}
	}
	PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);

	for (u32 i = 0; i < count; i++)
	{
		if (is_postgres_backend(pids[i]))
			continue;

		PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_EXCLUSIVE);
		PwhSharedMemoryBackendEntry *be =
			PWH_GET_BACKEND_ENTRY_UNSAFE(slot_indexes[i]);
		if (be->backend_pid == (sig_atomic_t) pids[i])
		{
			ereport(
				DEBUG1,
				(errmsg("PWH: Cleaning up orphaned slot %u", slot_indexes[i]),
				 errdetail("PID %u is not a PostgreSQL backend", pids[i])));
			pwh_release_backend_entry_unlocked(be);
			n_cleaned++;
		}
		PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);
	}

	pfree(slot_indexes);
	pfree(pids);

	return n_cleaned;
}

void
pwh_refresh_metrics(void)
{
	TimestampTz	  now = GetCurrentTimestamp();
	TimestampTz	  freshness_us = (TimestampTz) PWH_GUC_SIGNAL_TIMEOUT_MS * 1000;
	u32			 *pids;
	u32			 *slot_indexes;
	sig_atomic_t *generations;
	u32			  count = 0;
	u64			  observed_generation;
	u64			  refresh_token;

	pwh_cleanup_orphaned_slots();

	PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_EXCLUSIVE);
	observed_generation = PWH_SHMEM->refresh_generation;
	if (PWH_SHMEM->refresh_in_progress &&
		timestamp_is_fresh(now, PWH_SHMEM->last_refresh_time, freshness_us))
	{
		PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);
		for (i32 waited = 0; waited < PWH_GUC_SIGNAL_TIMEOUT_MS; waited++)
		{
			pg_usleep(1000L);
			PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_SHARED);
			bool finished =
				PWH_SHMEM->refresh_generation != observed_generation;
			PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);
			if (finished)
				break;
		}
		return;
	}
	if (timestamp_is_fresh(now, PWH_SHMEM->last_refresh_time, freshness_us))
	{
		PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);
		return;
	}

	PWH_SHMEM->refresh_in_progress = true;
	PWH_SHMEM->refresh_owner_pid = MyProcPid;
	PWH_SHMEM->refresh_token = PWH_SHMEM->refresh_token == UINT64_MAX
								   ? 1
								   : PWH_SHMEM->refresh_token + 1;
	refresh_token = PWH_SHMEM->refresh_token;
	PWH_SHMEM->last_refresh_time = now;
	pids = palloc(sizeof(u32) * PWH_GUC_MAX_TRACKED_QUERIES);
	slot_indexes = palloc(sizeof(u32) * PWH_GUC_MAX_TRACKED_QUERIES);
	generations = palloc(sizeof(sig_atomic_t) * PWH_GUC_MAX_TRACKED_QUERIES);
	for (u64 i = 0; i < (u64) PWH_GUC_MAX_TRACKED_QUERIES; i++)
	{
		PwhSharedMemoryBackendEntry *be = PWH_GET_BACKEND_ENTRY_UNSAFE(i);
		if (pwh_is_backend_entry_active(be))
		{
			pids[count] = be->backend_pid;
			slot_indexes[count] = (u32) i;
			generations[count] = be->poll_generation;
			count++;
		}
	}
	PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);

	for (u32 i = 0; i < count; i++)
		if (!is_postgres_backend(pids[i]) || !signal_process(pids[i], SIGUSR2))
			generations[i] = -1;

	for (i32 waited = 0; count > 0 && waited < PWH_GUC_SIGNAL_TIMEOUT_MS;
		 waited++)
	{
		u32 pending = 0;
		for (u32 i = 0; i < count; i++)
		{
			if (generations[i] < 0)
				continue;

			PwhSharedMemoryBackendEntry *be =
				PWH_GET_BACKEND_ENTRY_UNSAFE(slot_indexes[i]);
			if (be->backend_pid != (sig_atomic_t) pids[i] ||
				be->poll_generation != generations[i] ||
				!is_postgres_backend(pids[i]))
				generations[i] = -1;
			else
				pending++;
		}
		if (pending == 0)
			break;
		pg_usleep(1000L);
	}

	pfree(generations);
	pfree(slot_indexes);
	pfree(pids);
	PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_EXCLUSIVE);
	if (PWH_SHMEM->refresh_in_progress &&
		PWH_SHMEM->refresh_owner_pid == MyProcPid &&
		PWH_SHMEM->refresh_token == refresh_token)
	{
		PWH_SHMEM->last_refresh_time = GetCurrentTimestamp();
		PWH_SHMEM->refresh_in_progress = false;
		PWH_SHMEM->refresh_owner_pid = 0;
		PWH_SHMEM->refresh_generation++;
	}
	PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);
}

PwhMetricsSnapshot *
pwh_take_metrics_snapshot(void)
{
	PwhMetricsSnapshot *snapshot = palloc0(sizeof(PwhMetricsSnapshot));
	snapshot->entries =
		palloc0(sizeof(PwhSnapshotEntry) * PWH_GUC_MAX_TRACKED_QUERIES);

	PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_SHARED);
	for (u64 i = 0; i < (u64) PWH_GUC_MAX_TRACKED_QUERIES; i++)
	{
		PwhSharedMemoryBackendEntry *be = PWH_GET_BACKEND_ENTRY_UNSAFE(i);
		if (!pwh_is_backend_entry_active(be) || be->count_of_metrics == 0 ||
			be->count_of_metrics > (u32) PWH_GUC_MAX_NODES_PER_QUERY)
			continue;

		PwhSnapshotEntry *dst = &snapshot->entries[snapshot->count];
		for (u32 attempts = 0; attempts < 4; attempts++)
		{
			sig_atomic_t before = be->write_sequence;
			if ((before & 1) != 0)
				continue;
			dst->backend_pid = be->backend_pid;
			dst->owner_oid = be->owner_oid;
			dst->query_id = be->query_id;
			dst->query_start_time = be->query_start_time;
			dst->count_of_metrics = be->count_of_metrics;
			dst->query_text = palloc(PWH_GUC_MAX_QUERY_TEXT_LEN);
			dst->metrics =
				palloc(sizeof(PwhNodeMetrics) * dst->count_of_metrics);
			memcpy(dst->query_text, pwh_get_backend_entry_query_text(be),
				   PWH_GUC_MAX_QUERY_TEXT_LEN);
			memcpy(dst->metrics, pwh_get_backend_entry_metrics(be),
				   sizeof(PwhNodeMetrics) * dst->count_of_metrics);
			PWH_MEMORY_BARRIER();
			if (before == be->write_sequence && (before & 1) == 0 &&
				dst->backend_pid == be->backend_pid)
			{
				for (u32 j = 0; j < dst->count_of_metrics; j++)
					dst->total_query_time +=
						dst->metrics[j].execution.total_time_us;
				snapshot->count++;
				break;
			}
			pfree(dst->metrics);
			pfree(dst->query_text);
			dst->metrics = NULL;
			dst->query_text = NULL;
		}
	}
	PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);
	return snapshot;
}

void
pwh_free_metrics_snapshot(PwhMetricsSnapshot *snapshot)
{
	if (snapshot == NULL)
		return;

	for (u32 i = 0; i < snapshot->count; i++)
	{
		pfree(snapshot->entries[i].metrics);
		pfree(snapshot->entries[i].query_text);
	}
	pfree(snapshot->entries);
	pfree(snapshot);
}

static bool
is_postgres_backend(u32 pid)
{
	return BackendPidGetProc((int) pid) != NULL;
}

static bool
timestamp_is_fresh(TimestampTz now, TimestampTz then, TimestampTz freshness_us)
{
	return then != 0 && now >= then && now - then <= freshness_us;
}

static bool
signal_process(u32 pid, int sig)
{
	return kill((pid_t) pid, sig) == 0;
}
