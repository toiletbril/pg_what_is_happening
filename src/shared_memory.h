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
#include <stdint.h>

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
	i32		parent_node_id;

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
	Oid					  owner_oid;
	u64					  query_id;
	volatile sig_atomic_t poll_generation;
	volatile sig_atomic_t write_sequence;
	TimestampTz			  query_start_time;
	u32					  count_of_metrics;
	/* Query text and metrics follow after. */
} PwhSharedMemoryBackendEntry;

extern PwhSharedMemoryHeader *PWH_SHMEM;

extern Size PWH_SHMEM_SIZE;
extern Size PWH_BACKEND_ENTRY_STRIDE;

typedef struct
{
	i32				backend_pid;
	Oid				owner_oid;
	u64				query_id;
	TimestampTz		query_start_time;
	u32				count_of_metrics;
	char		   *query_text;
	PwhNodeMetrics *metrics;
	double			total_query_time;
} PwhSnapshotEntry;

typedef struct
{
	u32				  count;
	PwhSnapshotEntry *entries;
} PwhMetricsSnapshot;

#define PWH_GET_BACKEND_ENTRY_UNSAFE(idx)                                  \
	((PwhSharedMemoryBackendEntry *) ((char *) (PWH_SHMEM) +               \
									  MAXALIGN(                            \
										  sizeof(PwhSharedMemoryHeader)) + \
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
	be->poll_generation =
		be->poll_generation == SIG_ATOMIC_MAX ? 0 : be->poll_generation + 1;
	PWH_MEMORY_BARRIER();
}

forceinline bool
pwh_begin_metrics_write(PwhSharedMemoryBackendEntry *be,
						sig_atomic_t				*base_sequence)
{
	sig_atomic_t sequence = be->write_sequence;
	if ((sequence & 1) != 0)
		return false;
	if (sequence >= SIG_ATOMIC_MAX - 1)
		sequence = 0;
	be->write_sequence = sequence + 1;
	PWH_MEMORY_BARRIER();
	*base_sequence = sequence;
	return true;
}

forceinline void
pwh_end_metrics_write(PwhSharedMemoryBackendEntry *be,
					  sig_atomic_t				   base_sequence)
{
	PWH_MEMORY_BARRIER();
	be->write_sequence = base_sequence + 2;
}

forceinline void
pwh_advance_poll_generation(PwhSharedMemoryBackendEntry *be)
{
	be->poll_generation =
		be->poll_generation == SIG_ATOMIC_MAX ? 0 : be->poll_generation + 1;
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
extern void pwh_calculate_shared_memory_layout(void);

/* Cannot return NULL. */
extern PwhSharedMemoryBackendEntry *pwh_get_backend_entry(u64 index);

/* Cannot return NULL. */
extern char *pwh_get_backend_entry_query_text(
	PwhSharedMemoryBackendEntry *entry);
/* Cannot return NULL. */
extern PwhNodeMetrics *pwh_get_backend_entry_metrics(
	PwhSharedMemoryBackendEntry *entry);
extern bool pwh_validate_node_magic(PwhNodeMetrics *node, u32 node_id);
extern u32	pwh_cleanup_orphaned_slots(void);
extern void pwh_refresh_metrics(void);
extern PwhMetricsSnapshot *pwh_take_metrics_snapshot(void);
extern void pwh_free_metrics_snapshot(PwhMetricsSnapshot *snapshot);

#endif /* PWH_SHARED_MEMORY_H */
