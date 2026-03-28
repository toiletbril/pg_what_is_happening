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

/*
 * Calculate required shared memory size
 */
Size
pwh_shared_memory_size(void)
{
	Size size = 0;

	/* Control structure. */
	size = add_size(size, sizeof(PwhSharedMemoryHeader));

	/* Slots array (one per max_connections). */
	size = add_size(size,
					mul_size(MaxBackends, sizeof(PwhSharedMemoryBackendEntry)));

	return size;
}

/*
 * Initialize shared memory structures
 * Called from shmem_startup_hook
 */
void
pwh_shared_memory_startup(void)
{
	if (PREV_SHMEM_STARTUP_HOOK)
		PREV_SHMEM_STARTUP_HOOK();

	bool was_found;
	PWH_SHMEM = ShmemInitStruct("pg_what_is_happening",
								pwh_shared_memory_size(), &was_found);

	if (unlikely(!was_found))
	{
		elog(LOG,
			 "PWH: Initializing %zu bytes shared memory for %d backend entries",
			 pwh_shared_memory_size(), MaxBackends);

		for (u64 i = 0; i < (u64) MaxBackends; i++)
		{
			PwhSharedMemoryBackendEntry *shmem_be_entry =
				PWH_GET_BACKEND_ENTRY_UNSAFE(i);
			MemSet(shmem_be_entry, 0, sizeof(PwhSharedMemoryBackendEntry));
			shmem_be_entry->backend_pid = 0;
			shmem_be_entry->lock_offset = i;
		}

		PWH_LWLOCK_SETUP_TRANCHE(PWH_LWLOCK_TRANCHE_ID, "pg_what_is_happening");
		PWH_LWLOCK_INITIALIZE(PWH_SHMEM->lock, PWH_LWLOCK_TRANCHE_ID);
	}
}

PwhSharedMemoryBackendEntry *
pwh_get_my_backend_entry(void)
{
	if (unlikely(PWH_SHMEM == NULL))
		return NULL;

	for (u64 i = 0; i < (u64) MaxBackends; i++)
	{
		PwhSharedMemoryBackendEntry *be = pwh_get_backend_entry(i);

		if (be->backend_pid == MyProcPid)
		{
			return be;
		}
	}

	PWH_LWLOCK_ACQUIRE(PWH_SHMEM->lock, LW_EXCLUSIVE);
	for (u64 i = 0; i < (u64) MaxBackends; i++)
	{
		PwhSharedMemoryBackendEntry *be = pwh_get_backend_entry(i);

		if (be->backend_pid == 0)
		{
			be->backend_pid = MyProcPid;
			PWH_LWLOCK_RELEASE(PWH_SHMEM->lock);
			elog(DEBUG2, "PWH: Allocated backend entry %d for PID %d", i,
				 MyProcPid);

			return be;
		}
	}
	PWH_LWLOCK_RELEASE(PWH_SHMEM->lock);

	elog(WARNING, "PWH: All %d backend entries exhausted for PID %d",
		 MaxBackends, MyProcPid);

	return NULL;
}

/*
 * Get total number of backend entries.
 */
u64
pwh_get_backend_entry_count(void)
{
	return (u64) MaxBackends;
}

/*
 * Get backend entry by index.
 */
PwhSharedMemoryBackendEntry *
pwh_get_backend_entry(u64 index)
{
	if (unlikely(PWH_SHMEM == NULL))
	{
		elog(
			LOG,
			"Shared memory wasn't initialized when searching for backend entry");
		return NULL;
	}

	if (index >= (u64) MaxBackends)
		return NULL;

	return PWH_GET_BACKEND_ENTRY_UNSAFE(index);
}
