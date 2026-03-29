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

Size
pwh_get_backend_entry_stride(void)
{
	Size size = sizeof(PwhSharedMemoryBackendEntry);
	size = add_size(size, PWH_QUERY_TEXT_LEN_GUC);
	size =
		add_size(size, mul_size(PWH_MAX_NODES_PER_QUERY_GUC, sizeof(PwhNode)));
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
		elog(LOG,
			 "PWH: Initializing %zu bytes shared memory for %d backend entries",
			 pwh_get_shared_memory_size(), PWH_MAX_TRACKED_QUERIES_GUC);

		for (u64 i = 0; i < (u64) PWH_MAX_TRACKED_QUERIES_GUC; i++)
		{
			PwhSharedMemoryBackendEntry *shmem_be_entry =
				PWH_GET_BACKEND_ENTRY_UNSAFE(i);
			MemSet(shmem_be_entry, 0, pwh_get_backend_entry_stride());
			shmem_be_entry->backend_pid = 0;
			shmem_be_entry->lock_offset = i;
			shmem_be_entry->query_text_capacity = (u32) PWH_QUERY_TEXT_LEN_GUC;
			shmem_be_entry->plan_nodes_capacity =
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
			elog(DEBUG2, "PWH: Allocated backend entry %lu for PID %u", i,
				 MyProcPid);

			return be;
		}
	}
	PWH_LWLOCK_RELEASE(PWH_SHMEM->lock);

	elog(WARNING, "PWH: All %d backend entries exhausted for PID %d",
		 PWH_MAX_TRACKED_QUERIES_GUC, MyProcPid);

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
pwh_get_entry_query_text(PwhSharedMemoryBackendEntry *entry)
{
	return (char *) entry + sizeof(PwhSharedMemoryBackendEntry);
}

/*
 * Get pointer to plan_nodes array for a backend entry.
 */
PwhNode *
pwh_get_entry_plan_nodes(PwhSharedMemoryBackendEntry *entry)
{
	return (PwhNode *) ((char *) entry + sizeof(PwhSharedMemoryBackendEntry) +
						PWH_QUERY_TEXT_LEN_GUC);
}
