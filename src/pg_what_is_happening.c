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

#include "catalog/pg_type.h"
#include "common.h"
#include "compatibility.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "plan_tree_walker.h"
#include "shared_memory.h"
#include "signal_handler.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/timestamp.h"
#ifdef WITH_BGWORKER
#include "bg_worker.h"
#endif

PG_MODULE_MAGIC;

/* GUC variables. */
static bool PWH_ENABLED = true;
#ifdef WITH_BGWORKER
static char *PWH_LISTEN_ADDRESS = NULL;
#endif

i32 PWH_MAX_TRACKED_QUERIES_GUC = 128;
i32 PWH_MAX_NODES_PER_QUERY_GUC = 128;
i32 PWH_QUERY_TEXT_LEN_GUC = 1024;
i32 PWH_SIGNAL_TIMEOUT_MS_GUC = 10;

/* Previous hooks. */
static ExecutorStart_hook_type PREV_EXECUTOR_START_HOOK = NULL;
static ExecutorEnd_hook_type   PREV_EXECUTOR_END_HOOK = NULL;
extern shmem_startup_hook_type PREV_SHMEM_STARTUP_HOOK;

void _PG_init(void);
void _PG_fini(void);

static void pwh_executor_start_hook(QueryDesc *queryDesc, i32 eflags);
static void pwh_executor_end_hook(QueryDesc *queryDesc);

static bool pwh_check_positive_guc(int *newval, void **extra, GucSource source);

static bool
pwh_check_positive_guc(int *newval, void **extra, GucSource source)
{
	if (*newval <= 0)
	{
		GUC_check_errdetail("Value must be positive");
		return false;
	}
	return true;
}

#ifdef WITH_BGWORKER
static bool pwh_check_listen_address(char **newval, void **extra,
									 GucSource source);

static bool
pwh_check_listen_address(char **newval, void **extra, GucSource source)
{
	char *value = *newval;

	if (value == NULL || *value == '\0')
	{
		GUC_check_errdetail("Listen address cannot be empty");
		return false;
	}

	char *colon = strchr(value, ':');
	if (colon == NULL)
	{
		GUC_check_errdetail("Listen address must be in format host:port");
		return false;
	}

	u64 host_len = colon - value;
	if (host_len == 0)
	{
		GUC_check_errdetail("Host part cannot be empty");
		return false;
	}

	if (host_len > 255)
	{
		GUC_check_errdetail("Host part too long (max 255 characters)");
		return false;
	}

	char *endptr;
	long  port = strtol(colon + 1, &endptr, 10);

	if (*endptr != '\0' || endptr == colon + 1)
	{
		GUC_check_errdetail("Port must be a numeric value");
		return false;
	}

	if (port < 1 || port > 65535)
	{
		GUC_check_errdetail("Port must be between 1 and 65535");
		return false;
	}

	return true;
}
#endif

PWH_SHMEM_REQUEST_HOOK_DECL;

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		elog(
			ERROR,
			"pg_what_is_happening must be loaded via shared_preload_libraries");

	PWH_SHMEM = NULL;

	/* Define GUC variables. */
	DefineCustomBoolVariable(
		"pg_what_is_happening.enabled", "Enable pg_what_is_happening extension",
		NULL, &PWH_ENABLED, true, PGC_SIGHUP, 0, NULL, NULL, NULL);

#ifdef WITH_BGWORKER
	DefineCustomStringVariable(
		"pg_what_is_happening.listen_address",
		"Listen address for metrics endpoint (host:port)", NULL,
		&PWH_LISTEN_ADDRESS, "127.0.0.1:9187", PGC_POSTMASTER, 0,
		pwh_check_listen_address, NULL, NULL);
#endif

	DefineCustomIntVariable("pg_what_is_happening.max_tracked_queries",
							"Maximum number of concurrent queries to track",
							NULL, &PWH_MAX_TRACKED_QUERIES_GUC, 128, 1, 65536,
							PGC_POSTMASTER, 0, pwh_check_positive_guc, NULL,
							NULL);

	DefineCustomIntVariable("pg_what_is_happening.max_nodes_per_query",
							"Maximum plan nodes tracked per query", NULL,
							&PWH_MAX_NODES_PER_QUERY_GUC, 128, 16, 1024,
							PGC_POSTMASTER, 0, pwh_check_positive_guc, NULL,
							NULL);

	DefineCustomIntVariable(
		"pg_what_is_happening.query_text_len",
		"Maximum query text length to store", NULL, &PWH_QUERY_TEXT_LEN_GUC,
		1024, 64, 8192, PGC_POSTMASTER, 0, pwh_check_positive_guc, NULL, NULL);

	DefineCustomIntVariable("pg_what_is_happening.signal_timeout_ms",
							"Timeout waiting for signal handler response", NULL,
							&PWH_SIGNAL_TIMEOUT_MS_GUC, 10, 1, 1000, PGC_SIGHUP,
							0, NULL, NULL, NULL);

	/* Install hooks. */
	PWH_INSTALL_SHMEM_REQUEST_HOOK();

	PREV_SHMEM_STARTUP_HOOK = shmem_startup_hook;
	shmem_startup_hook = pwh_shared_memory_startup_hook;

	PREV_EXECUTOR_START_HOOK = ExecutorStart_hook;
	ExecutorStart_hook = pwh_executor_start_hook;

	PREV_EXECUTOR_END_HOOK = ExecutorEnd_hook;
	ExecutorEnd_hook = pwh_executor_end_hook;

	pwh_install_signal_handler();
#ifdef WITH_BGWORKER
	pwh_register_openmetrics_worker();
#endif

	elog(LOG, "pg_what_is_happening initialized");
}

void
_PG_fini(void)
{
	/* Restore hooks. */
	shmem_startup_hook = PREV_SHMEM_STARTUP_HOOK;
	ExecutorStart_hook = PREV_EXECUTOR_START_HOOK;
	ExecutorEnd_hook = PREV_EXECUTOR_END_HOOK;

	elog(LOG, "pg_what_is_happening unloaded");
}

static u64
get_query_id(const QueryDesc *qd)
{
	u64 id = PWH_GET_QUERY_ID(qd->plannedstmt);

	/* Fallback to hash if queryId is 0 (not populated without
	 * pg_stat_statements). */
	if (id == 0)
	{
		id = pwh_compute_query_id(qd->sourceText);
	}

	return id;
}

static void
pwh_executor_start_hook(QueryDesc *queryDesc, i32 eflags)
{
	static bool handler_was_installed = false;

	/* Attach to shared memory if not already attached. */
	PWH_SHMEM = pwh_get_shared_memory_ptr();

	/* Install signal handler once per backend. */
	if (!handler_was_installed)
	{
		pwh_install_signal_handler();
		handler_was_installed = true;
	}

	elog(LOG, "PWH: ExecutorStart hook called PID=%d enabled=%d", MyProcPid,
		 PWH_ENABLED);

	/* Force instrumentation on all nodes. */
	queryDesc->instrument_options |= INSTRUMENT_ALL;

	if (PREV_EXECUTOR_START_HOOK)
		PREV_EXECUTOR_START_HOOK(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (unlikely(!PWH_ENABLED))
	{
		elog(LOG, "PWH: ExecutorStart disabled, returning");
		return;
	}

	/* Get our backend entry. */
	PwhSharedMemoryBackendEntry *shmem_be_entry = pwh_get_my_backend_entry();
	if (unlikely(shmem_be_entry == NULL))
	{
		elog(WARNING, "PWH: Could not allocate backend entry for PID %d",
			 MyProcPid);
		return;
	}

	PwhNode *plan_nodes = pwh_get_entry_plan_nodes(shmem_be_entry);
	char	*query_text = pwh_get_entry_query_text(shmem_be_entry);

	u64 num_nodes =
		pwh_walk_plan_topology(queryDesc->planstate, plan_nodes,
							   (u64) PWH_MAX_NODES_PER_QUERY_GUC, -1);

	shmem_be_entry->is_query_active = true;

	elog(DEBUG1, "PWH: Tracking query with %zu nodes for PID %d", num_nodes,
		 MyProcPid);

	shmem_be_entry->num_nodes = (u32) num_nodes;
	shmem_be_entry->query_start_time = GetCurrentTimestamp();
	shmem_be_entry->query_id = get_query_id(queryDesc);

	/* Copy query text (truncated to buffer size). */
	if (queryDesc->sourceText)
		snprintf(query_text, PWH_QUERY_TEXT_LEN_GUC, "%s",
				 queryDesc->sourceText);
	else
		query_text[0] = '\0';

	elog(
		LOG,
		"PWH: ExecutorStart PID=%lu query_id=%lu num_nodes=%d shmem=%p query='%s'",
		MyProcPid, shmem_be_entry->query_id, num_nodes, PWH_SHMEM, query_text);

	/* Store QueryDesc for signal handler. */
	pwh_set_current_query_desc(queryDesc);
}

static void
pwh_executor_end_hook(QueryDesc *queryDesc)
{
	elog(
		LOG,
		"PWH: ExecutorEnd PID=%d calls=%ld success=%ld no_qd=%ld shm_null=%ld no_slot=%ld",
		MyProcPid, (long) pwh_get_signal_handler_call_count(),
		(long) pwh_get_signal_handler_success_count(),
		(long) pwh_get_signal_handler_no_querydesc(),
		(long) pwh_get_signal_handler_shmem_null(),
		(long) pwh_get_signal_handler_no_slot());

	if (likely(PWH_ENABLED))
	{
		/* Get our backend entry. */
		PwhSharedMemoryBackendEntry *shmem_be_entry =
			pwh_get_my_backend_entry();
		if (likely(shmem_be_entry && shmem_be_entry->is_query_active))
		{
			PwhNode *plan_nodes = pwh_get_entry_plan_nodes(shmem_be_entry);

			/* Capture final instrumentation. */
			pwh_walk_plan_instrumentation(queryDesc->planstate, plan_nodes,
										  PWH_MAX_NODES_PER_QUERY_GUC);

			elog(DEBUG1,
				 "PWH: Completed query tracking for PID %d (generation %lu)",
				 MyProcPid, (unsigned long) shmem_be_entry->poll_generation);

			/* Mark inactive, clear pid, and increment generation. */
			shmem_be_entry->is_query_active = false;
			shmem_be_entry->backend_pid = 0;
			shmem_be_entry->poll_generation++;
		}

		/* Clear QueryDesc pointer. */
		pwh_set_current_query_desc(NULL);
	}

	/* Call previous hook or standard function. */
	if (PREV_EXECUTOR_END_HOOK)
		PREV_EXECUTOR_END_HOOK(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

static TupleDesc
make_v1_status_tupdesc(void)
{
	TupleDesc tupdesc = PWH_CREATE_TUPLE_DESC(17);

	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "backend_pid", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "query_id", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "query_text", TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "is_query_active", BOOLOID, -1,
					   0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "node_id", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "parent_node_id", INT4OID, -1,
					   0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "node_tag", TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 8, "tuples_returned", FLOAT8OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 9, "startup_time_us", FLOAT8OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 10, "total_time_us", FLOAT8OID, -1,
					   0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 11, "loops_executed", FLOAT8OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 12, "cache_hits", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 13, "cache_misses", INT8OID, -1,
					   0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 14, "local_cache_hits", INT8OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 15, "local_cache_misses", INT8OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 16, "spill_file_reads", INT8OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 17, "spill_file_writes", INT8OID,
					   -1, 0);

	return tupdesc;
}

PG_FUNCTION_INFO_V1(v1_status_f);

Datum
v1_status_f(PG_FUNCTION_ARGS)
{
	if (SRF_IS_FIRSTCALL())
	{
		FuncCallContext *funcctx = SRF_FIRSTCALL_INIT();
		MemoryContext	 oldcontext =
			MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		TupleDesc td = make_v1_status_tupdesc();
		PWH_TUPLE_DESC_FINALIZE(td);
		funcctx->tuple_desc = BlessTupleDesc(td);

		/* Send SIGUSR2 to all active backends to refresh metrics. */
		u32 signaled = pwh_request_backend_metrics();

		elog(DEBUG1, "PWH: What_is_happening() called, signaled %u backends",
			 signaled);

		/* Wait for backends to refresh metrics. */
		pg_usleep(PWH_SIGNAL_TIMEOUT_MS_GUC * 1000L);

		/* Initialize state: we iterate through all slots and nodes. */
		u32 *state = (u32 *) palloc(2 * sizeof(i32));
		state[0] = 0; /* slot index. */
		state[1] = 0; /* node index. */
		funcctx->user_fctx = state;

		MemoryContextSwitchTo(oldcontext);
	}

	FuncCallContext *funcctx = SRF_PERCALL_SETUP();

	u32 *state = (u32 *) funcctx->user_fctx;
	u32	 slot_idx = state[0];
	u32	 node_idx = state[1];

	/* Find next valid backend entry+node combination. */
	while (slot_idx < (u32) pwh_get_backend_entry_count())
	{
		PwhSharedMemoryBackendEntry *shmem_be_entry =
			pwh_get_backend_entry(slot_idx);
		if (shmem_be_entry && shmem_be_entry->backend_pid != 0 &&
			node_idx < shmem_be_entry->num_nodes)
		{
			PwhNode *plan_nodes = pwh_get_entry_plan_nodes(shmem_be_entry);
			char	*query_text = pwh_get_entry_query_text(shmem_be_entry);
			PwhNode *node = &plan_nodes[node_idx];

			/* Build result row. */
			Datum values[17];
			bool  nulls[17];
			MemSet(nulls, 0, sizeof(nulls));

			values[0] = Int32GetDatum(shmem_be_entry->backend_pid);
			values[1] = Int64GetDatum(shmem_be_entry->query_id);
			values[2] = CStringGetTextDatum(query_text);
			values[3] = BoolGetDatum(shmem_be_entry->is_query_active);
			values[4] = Int32GetDatum(node->node_id);
			values[5] = Int32GetDatum(node->parent_node_id);
			values[6] = CStringGetTextDatum(pwh_node_tag_to_string(node->tag));
			values[7] = Float8GetDatum(node->execution.tuples_returned);
			values[8] = Float8GetDatum(node->execution.startup_time_us);
			values[9] = Float8GetDatum(node->execution.total_time_us);
			values[10] = Float8GetDatum(node->execution.loops_executed);
			values[11] = Int64GetDatum(node->buffer_usage.cache_hits);
			values[12] = Int64GetDatum(node->buffer_usage.cache_misses);
			values[13] = Int64GetDatum(node->buffer_usage.local_cache_hits);
			values[14] = Int64GetDatum(node->buffer_usage.local_cache_misses);
			values[15] = Int64GetDatum(node->buffer_usage.spill_file_reads);
			values[16] = Int64GetDatum(node->buffer_usage.spill_file_writes);

			HeapTuple tuple =
				heap_form_tuple(funcctx->tuple_desc, values, nulls);

			/* Advance to next node. */
			node_idx++;
			state[1] = node_idx;

			SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
		}

		/* Move to next slot. */
		slot_idx++;
		node_idx = 0;
		state[0] = slot_idx;
		state[1] = 0;
	}

	SRF_RETURN_DONE(funcctx);
}
