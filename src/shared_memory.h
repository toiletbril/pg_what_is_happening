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

#ifndef PWH_SHARED_MEMORY_H
#define PWH_SHARED_MEMORY_H

#include "postgres.h"

#include <signal.h>

#include "common.h"
#include "compatibility.h"
#include "datatype/timestamp.h"
#include "nodes/nodeFuncs.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/spin.h"

/*
 * TODO: Test socket/pipe approach as alternative to shared memory.
 */

#define PWH_NODE_MAGIC 0xDEADBEEF

extern shmem_startup_hook_type PREV_SHMEM_STARTUP_HOOK;

typedef struct
{
	NodeTag tag;
	u32		node_id;
	u32		parent_node_id;

	struct
	{
		double tuples_returned;
		double loops_executed;
		double startup_time_us;
		double total_time_us;
		double rows_filtered_by_joins;
		double rows_filtered_by_expressions;
	} execution;

	struct
	{
		u64 cache_hits;
		u64 cache_misses;
		u64 local_cache_hits;
		u64 local_cache_misses;
		u64 spill_file_reads;
		u64 spill_file_writes;
	} buffer_usage;

	u64 magic;
} PwhNodeMetrics;

typedef struct
{
	volatile sig_atomic_t backend_pid;
	u64					  query_id;
	u32					  poll_generation;
	TimestampTz			  query_start_time;
	u32					  count_of_metrics;
	/* Query text and metrics follow after. */
} PwhSharedMemoryBackendEntry;

extern PwhSharedMemoryHeader *PWH_SHMEM;

extern Size PWH_SHMEM_SIZE;
extern Size PWH_BACKEND_ENTRY_STRIDE;

#define PWH_GET_BACKEND_ENTRY_UNSAFE(idx)                             \
	((PwhSharedMemoryBackendEntry *) ((char *) (PWH_SHMEM) +          \
									  sizeof(PwhSharedMemoryHeader) + \
									  ((idx) * PWH_BACKEND_ENTRY_STRIDE)))

/* Can return NULL. */
extern PwhSharedMemoryBackendEntry *pwh_get_or_create_my_backend_entry_impl(
	bool should_create, bool should_acquire_lock);

forceinline bool
pwh_is_backend_entry_active(const PwhSharedMemoryBackendEntry *be)
{
	Assert(be != NULL);
	return be->backend_pid != 0;
}

forceinline void
pwh_release_backend_entry_unlocked(PwhSharedMemoryBackendEntry *be)
{
	Assert(pwh_is_backend_entry_active(be));
	be->backend_pid = 0;
	be->poll_generation++;
	PWH_MEMORY_BARRIER();
}

/* Can return NULL. */
forceinline PwhSharedMemoryBackendEntry *
pwh_get_or_create_my_backend_entry(void)
{
	return pwh_get_or_create_my_backend_entry_impl(true, true);
}

/* Can return NULL. */
forceinline PwhSharedMemoryBackendEntry *
pwh_get_my_backend_entry(void)
{
	return pwh_get_or_create_my_backend_entry_impl(false, true);
}

/* Can return NULL. */
forceinline PwhSharedMemoryBackendEntry *
pwh_get_my_backend_entry_unlocked(void)
{
	return pwh_get_or_create_my_backend_entry_impl(false, false);
}

extern void pwh_release_my_backend_entry(void);

/* Cannot return NULL. */
void *pwh_get_shared_memory_ptr(void);

extern void pwh_shared_memory_startup_hook(void);

/* Cannot return NULL. */
extern PwhSharedMemoryBackendEntry *pwh_get_backend_entry(u64 index);

/* Cannot return NULL. */
extern char *pwh_get_backend_entry_query_text(
	PwhSharedMemoryBackendEntry *entry);
/* Cannot return NULL. */
extern PwhNodeMetrics *pwh_get_backend_entry_metrics(
	PwhSharedMemoryBackendEntry *entry);
extern u32	pwh_request_backend_metrics_unlocked(void);
extern bool pwh_validate_node_magic(PwhNodeMetrics *node, u32 node_id);
extern u32	pwh_cleanup_orphaned_slots(void);

#endif /* PWH_SHARED_MEMORY_H */
