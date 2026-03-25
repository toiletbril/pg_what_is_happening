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

#define PWH_MAX_NODES_DEFAULT 128
#define PWH_QUERY_TEXT_LEN 256
#define PWH_NODE_TYPE_NAME_LEN 32

typedef struct
{
	NodeTag tag;
	i32		node_id;
	i32		parent_node_id;
	char	node_type_name[PWH_NODE_TYPE_NAME_LEN];

	struct
	{
		double tuples_returned;
		double loops_executed;
		double startup_time_us;
		double total_time_us;
	} execution;

	struct
	{
		i64 shared_hit;
		i64 shared_read;
		i64 local_hit;
		i64 local_read;
		i64 temp_read;
		i64 temp_written;
	} buffer_usage;
} PwhNode;

typedef struct
{
	i32			backend_pid;
	u64			query_id;
	u64			poll_generation;
	char		query_text[PWH_QUERY_TEXT_LEN];
	TimestampTz query_start_time;
	bool		is_query_active;
	i32			num_nodes;
	i32			lock_offset;

	PwhNode plan_nodes[PWH_MAX_NODES_DEFAULT];
} PwhSharedMemoryBackendEntry;

extern PwhSharedMemoryHeader *PWH_SHMEM;

#define PWH_GET_BACKEND_ENTRY(idx)                                       \
	(((PwhSharedMemoryBackendEntry *) ((char *) (PWH_SHMEM) +            \
									   sizeof(PwhSharedMemoryHeader))) + \
	 (idx))

extern Size							pwh_shared_memory_size(void);
extern void							pwh_shared_memory_startup(void);
extern PwhSharedMemoryBackendEntry *pwh_get_my_backend_entry(void);
extern i32							pwh_get_backend_entry_count(void);
extern PwhSharedMemoryBackendEntry *pwh_get_backend_entry(i32 index);

#endif /* PWH_SHARED_MEMORY_H. */
