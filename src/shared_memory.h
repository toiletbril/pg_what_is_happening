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
#include "storage/lwlock.h"
#include "storage/spin.h"

/* TODO: Test socket/pipe approach as alternative to shared memory.
 * While shared memory is the only practical PostgreSQL IPC for metrics,
 * investigate if domain sockets could reduce synchronization complexity.
 */

#define PWH_NODE_MAGIC 0xDEADBEEF

extern i32 PWH_MAX_TRACKED_QUERIES_GUC;
extern i32 PWH_MAX_NODES_PER_QUERY_GUC;
extern i32 PWH_QUERY_TEXT_LEN_GUC;
extern i32 PWH_SIGNAL_TIMEOUT_MS_GUC;

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
	u32					  num_nodes;
	/* Query text and metrics follow after. */
} PwhSharedMemoryBackendEntry;

extern PwhSharedMemoryHeader *PWH_SHMEM;

Size pwh_get_backend_entry_stride(void);

#define PWH_GET_BACKEND_ENTRY_UNSAFE(idx)                             \
	((PwhSharedMemoryBackendEntry *) ((char *) (PWH_SHMEM) +          \
									  sizeof(PwhSharedMemoryHeader) + \
									  ((idx) *                        \
									   pwh_get_backend_entry_stride())))

void							   *pwh_get_shared_memory_ptr(void);
extern Size							pwh_get_shared_memory_size(void);
extern void							pwh_shared_memory_startup_hook(void);
extern PwhSharedMemoryBackendEntry *pwh_get_or_create_my_backend_entry(void);
extern u64							pwh_get_backend_entry_count(void);
extern PwhSharedMemoryBackendEntry *pwh_get_backend_entry(u64 index);
extern char						   *pwh_get_backend_entry_query_text(
						   PwhSharedMemoryBackendEntry *entry);
extern PwhNodeMetrics *pwh_get_backend_entry_metrics(
	PwhSharedMemoryBackendEntry *entry);
extern u32	pwh_request_backend_metrics_unlocked(void);
extern bool pwh_validate_node_magic(PwhNodeMetrics *node, u32 node_id);
extern u32	pwh_cleanup_orphaned_slots(void);

#endif /* PWH_SHARED_MEMORY_H */
