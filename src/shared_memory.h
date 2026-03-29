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

#include "common.h"
#include "compatibility.h"
#include "datatype/timestamp.h"
#include "nodes/nodeFuncs.h"
#include "storage/lwlock.h"

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
	} execution;

	struct
	{
		u64 shared_hit;
		u64 shared_read;
		u64 local_hit;
		u64 local_read;
		u64 temp_read;
		u64 temp_written;
	} buffer_usage;

	u64 magic;
} PwhNode;

typedef struct
{
	i32			backend_pid;
	u64			query_id;
	u32			poll_generation;
	TimestampTz query_start_time;
	bool		is_query_active;
	u32			num_nodes;
	u32			lock_offset;
	u32			query_text_capacity;
	u32			plan_nodes_capacity;
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
extern PwhSharedMemoryBackendEntry *pwh_get_my_backend_entry(void);
extern u64							pwh_get_backend_entry_count(void);
extern PwhSharedMemoryBackendEntry *pwh_get_backend_entry(u64 index);
extern char	   *pwh_get_entry_query_text(PwhSharedMemoryBackendEntry *entry);
extern PwhNode *pwh_get_entry_plan_nodes(PwhSharedMemoryBackendEntry *entry);

#endif /* PWH_SHARED_MEMORY_H. */
