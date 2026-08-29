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

#ifndef PWH_COMPATIBILITY_H
#define PWH_COMPATIBILITY_H

#include "postgres.h"

#include "common.h"
#include "executor/execdesc.h"
#include "float.h"
#include "nodes/nodes.h"
#if PG_VERSION_NUM >= 100000 && PG_VERSION_NUM < 110000
#include "port/atomics.h"
#else
#include "storage/barrier.h"
#endif
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

/* Greenplum detection. */
#ifdef GP_VERSION_NUM
#define PWH_IS_GREENPLUM 1
#else
#define PWH_IS_GREENPLUM 0
#endif

/* Forward declarations for compatibility headers. */
typedef struct PlanState PlanState;

/* Include version-specific compatibility definitions. */
#if PG_VERSION_NUM >= 190000
#include "compatibility/19.h"
#elif PG_VERSION_NUM >= 150000
#include "compatibility/15-18.h"
#elif PG_VERSION_NUM >= 140000
#include "compatibility/14.h"
#elif PG_VERSION_NUM >= 130000
#include "compatibility/13.h"
#elif PG_VERSION_NUM >= 120000
#include "compatibility/12.h"
#elif PG_VERSION_NUM >= 110000
#include "compatibility/11.h"
#elif PG_VERSION_NUM >= 100000
#include "compatibility/10.h"
#elif PG_VERSION_NUM >= 90600
#include "compatibility/9_6.h"
#elif PG_VERSION_NUM >= 90500
#include "compatibility/9_5.h"
#else
#include "compatibility/9_4.h"
#endif

#if PG_VERSION_NUM >= 190000
typedef NodeInstrumentation PwhNodeInstrumentation;
#define PWH_BASE_INSTRUMENTATION(node_instr) (&(node_instr)->instr)
#define PWH_INSTR_TOTAL(node_instr) ((node_instr)->instr.total)
#define PWH_INSTR_SHARED_HITS(node_instr) \
	((node_instr)->instr.bufusage.shared_blks_hit)
#define PWH_SIG_IGN PG_SIG_IGN
#define PWH_SIG_DFL PG_SIG_DFL
#define PWH_CALL_SIGNAL_HANDLER(handler) \
	(handler)(postgres_signal_arg, pg_siginfo)
#else
typedef Instrumentation PwhNodeInstrumentation;
#define PWH_BASE_INSTRUMENTATION(node_instr) (node_instr)
#define PWH_INSTR_TOTAL(node_instr) ((node_instr)->total)
#define PWH_INSTR_SHARED_HITS(node_instr) \
	((node_instr)->bufusage.shared_blks_hit)
#define PWH_SIG_IGN SIG_IGN
#define PWH_SIG_DFL SIG_DFL
#define PWH_CALL_SIGNAL_HANDLER(handler) (handler)(postgres_signal_arg)
#endif

#if PG_VERSION_NUM >= 190000
#define PWH_INSTR_TIME_MAYBE_GET_DOUBLE(n) (INSTR_TIME_GET_DOUBLE(n))
#else
#define PWH_INSTR_TIME_MAYBE_GET_DOUBLE(n) (n)
#endif

#if PG_VERSION_NUM >= 100000
typedef struct
{
	LWLock		entry_search_lock;
	bool		refresh_in_progress;
	i32			refresh_owner_pid;
	u8			__pad[3];
	u64			refresh_token;
	u64			refresh_generation;
	TimestampTz last_refresh_time;
} PwhSharedMemoryHeader;
#else
typedef struct
{
	LWLock	   *entry_search_lock;
	bool		refresh_in_progress;
	i32			refresh_owner_pid;
	u8			__pad[3];
	u64			refresh_token;
	u64			refresh_generation;
	TimestampTz last_refresh_time;
} PwhSharedMemoryHeader;
#endif

extern u64 pwh_compute_query_id(const QueryDesc *qd);

extern const char *pwh_node_tag_to_string(NodeTag tag);

extern pqsigfunc pwh_install_pqsignal(int signo, pqsigfunc func);
extern bool		 pwh_is_regular_backend(void);

#define PWH_MEMORY_BARRIER() pg_memory_barrier()

#ifdef XACT_EVENT_PARALLEL_ABORT
#define PWH_IS_ABORT_EVENT(event) \
	((event) == XACT_EVENT_ABORT || (event) == XACT_EVENT_PARALLEL_ABORT)
#else
#define PWH_IS_ABORT_EVENT(event) ((event) == XACT_EVENT_ABORT)
#endif

#endif /* PWH_COMPATIBILITY_H */
