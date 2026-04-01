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

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "common.h"
#include "compatibility.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "metrics.h"
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
static bool PWH_IS_ENABLED_GUC = true;
#ifdef WITH_BGWORKER
static char *PWH_LISTEN_ADDRESS_GUC = NULL;
#endif

i32 PWH_MAX_TRACKED_QUERIES_GUC = 128;
i32 PWH_MAX_NODES_PER_QUERY_GUC = 128;
i32 PWH_QUERY_TEXT_LEN_GUC = 1024;
i32 PWH_SIGNAL_TIMEOUT_MS_GUC = 10;

/* Previous hooks. */
static ExecutorStart_hook_type PREV_QUERY_START_HOOK = NULL;
static ExecutorEnd_hook_type   PREV_QUERY_FINISH_HOOK = NULL;
extern shmem_startup_hook_type PREV_SHMEM_STARTUP_HOOK;

void _PG_init(void);
void _PG_fini(void);

static void query_start_hook(QueryDesc *queryDesc, i32 eflags);
static void query_end_hook(QueryDesc *queryDesc);
static void query_cleanup_callback(XactEvent event, void *arg);
static u64	get_query_id(const QueryDesc *qd);
static bool check_positive_guc(int *newval, void **extra, GucSource source);

#ifdef WITH_BGWORKER
static bool check_listen_address(char **newval, void **extra, GucSource source);
#endif

PWH_SHMEM_REQUEST_HOOK_DECL;

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		ereport(
			ERROR,
			(errcode(ERRCODE_CONFIG_FILE_ERROR),
			 errmsg(
				 "PWH: Extension must be loaded via shared_preload_libraries")));
	}

	PWH_SHMEM = NULL;

	/* Define GUC variables. */
	DefineCustomBoolVariable(
		"pg_what_is_happening.enabled", "Enable pg_what_is_happening extension",
		NULL, &PWH_IS_ENABLED_GUC, true, PGC_SIGHUP, 0, NULL, NULL, NULL);

#ifdef WITH_BGWORKER
	DefineCustomStringVariable(
		"pg_what_is_happening.listen_address",
		"Listen address for metrics endpoint (host:port)", NULL,
		&PWH_LISTEN_ADDRESS_GUC, "127.0.0.1:9187", PGC_POSTMASTER, 0,
		check_listen_address, NULL, NULL);
#endif

	DefineCustomIntVariable("pg_what_is_happening.max_tracked_queries",
							"Maximum number of concurrent queries to track",
							NULL, &PWH_MAX_TRACKED_QUERIES_GUC, 128, 1, 65536,
							PGC_POSTMASTER, 0, check_positive_guc, NULL, NULL);

	DefineCustomIntVariable("pg_what_is_happening.max_nodes_per_query",
							"Maximum plan nodes tracked per query", NULL,
							&PWH_MAX_NODES_PER_QUERY_GUC, 128, 16, 1024,
							PGC_POSTMASTER, 0, check_positive_guc, NULL, NULL);

	DefineCustomIntVariable("pg_what_is_happening.query_text_len",
							"Maximum query text length to store", NULL,
							&PWH_QUERY_TEXT_LEN_GUC, 1024, 64, 8192,
							PGC_POSTMASTER, 0, check_positive_guc, NULL, NULL);

	DefineCustomIntVariable("pg_what_is_happening.signal_timeout_ms",
							"Timeout waiting for signal handler response", NULL,
							&PWH_SIGNAL_TIMEOUT_MS_GUC, 10, 1, 1000, PGC_SIGHUP,
							0, NULL, NULL, NULL);

	/* Install hooks. */
	PWH_INSTALL_SHMEM_REQUEST_HOOK();

	PREV_SHMEM_STARTUP_HOOK = shmem_startup_hook;
	shmem_startup_hook = pwh_shared_memory_startup_hook;

	PREV_QUERY_START_HOOK = ExecutorStart_hook;
	ExecutorStart_hook = query_start_hook;

	PREV_QUERY_FINISH_HOOK = ExecutorEnd_hook;
	ExecutorEnd_hook = query_end_hook;

	pwh_install_signal_handler();
#ifdef WITH_BGWORKER
	pwh_register_openmetrics_exporter_as_bg_worker();
#endif

	ereport(LOG, (errmsg("PWH: Extension initialized")));
}

void
_PG_fini(void)
{
	/* Restore hooks. */
	shmem_startup_hook = PREV_SHMEM_STARTUP_HOOK;
	ExecutorStart_hook = PREV_QUERY_START_HOOK;
	ExecutorEnd_hook = PREV_QUERY_FINISH_HOOK;

	ereport(LOG, (errmsg("PWH: Extension unloaded")));
}

static bool
check_positive_guc(int *newval, void **extra, GucSource source)
{
	if (*newval <= 0)
	{
		GUC_check_errdetail("Value must be positive");
		return false;
	}
	return true;
}

#ifdef WITH_BGWORKER
static bool
check_listen_address(char **newval, void **extra, GucSource source)
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

/*
 * Cleanup callback invoked on transaction abort or commit.
 * Ensures CURRENT_QUERY_DESC is cleared even when ExecutorEnd_hook is not
 * called.
 */
static void
query_cleanup_callback(XactEvent event, void *arg)
{
	unused(arg);

	/* Only clean up on abort - commit path goes through ExecutorEnd normally.
	 */
	if (!PWH_IS_ABORT_EVENT(event))
		return;

	/* Clear QueryDesc pointer to prevent dangling reference. */
	pwh_set_current_query_desc(NULL);

	if (!PWH_IS_ENABLED_GUC)
		return;

	/* Get our backend entry and mark query as inactive. */
	PwhSharedMemoryBackendEntry *shmem_be_entry = pwh_get_my_backend_entry();
	if (shmem_be_entry != NULL && shmem_be_entry->backend_pid != 0)
	{
		PWH_MEMORY_BARRIER();
		shmem_be_entry->poll_generation++;

		ereport(DEBUG1,
				(errmsg("PWH: Cleaned up query state on abort for PID %d",
						MyProcPid),
				 errdetail("Generation: %lu",
						   (unsigned long) shmem_be_entry->poll_generation)));
	}
}

volatile bool WAS_BACKEND_INITIALIZED = false;

static void
initialize_state_once_per_backend(void)
{
	/* Query backend state on error. */
	RegisterXactCallback(query_cleanup_callback, NULL);
	/* Report metrics when signaled. */
	pwh_install_signal_handler();
	/* Communicate with BG worker. */
	PWH_SHMEM = pwh_get_shared_memory_ptr();

	WAS_BACKEND_INITIALIZED = true;
}

static void
query_start_hook(QueryDesc *queryDesc, i32 eflags)
{
	if (!WAS_BACKEND_INITIALIZED)
	{
		initialize_state_once_per_backend();
	}

	ereport(DEBUG2,
			(errmsg("PWH: ExecutorStart hook called"),
			 errdetail("PID=%d enabled=%d", MyProcPid, PWH_IS_ENABLED_GUC)));

	/* Force instrumentation on all nodes. */
	queryDesc->instrument_options |= INSTRUMENT_ALL;

	if (PREV_QUERY_START_HOOK)
	{
		PREV_QUERY_START_HOOK(queryDesc, eflags);
	}
	else
	{
		standard_ExecutorStart(queryDesc, eflags);
	}

	if (unlikely(!PWH_IS_ENABLED_GUC))
	{
		ereport(DEBUG2, (errmsg("PWH: ExecutorStart disabled, returning")));
		return;
	}

	MemoryContext old_context = CurrentMemoryContext;

	PG_TRY();
	{
		/* Get our backend entry. */
		PwhSharedMemoryBackendEntry *shmem_be_entry =
			pwh_get_my_backend_entry();
		if (unlikely(shmem_be_entry == NULL))
		{
			ereport(LOG, (errmsg("PWH: Could not allocate backend entry"),
						  errdetail("PID %d exhausted all slots", MyProcPid)));
		}
		else
		{
			PwhNodeMetrics *metrics =
				pwh_get_backend_entry_metrics(shmem_be_entry);
			char *query_text = pwh_get_backend_entry_query_text(shmem_be_entry);

			/* Set initial backend state and prepare for metric collection. */
			u64 num_nodes = pwh_walk_plan_topology(
				queryDesc->planstate, metrics, PWH_MAX_NODES_PER_QUERY_GUC, -1);

			ereport(DEBUG1, (errmsg("PWH: Tracking query with %lu nodes",
									(unsigned long) num_nodes),
							 errdetail("PID %d", MyProcPid)));

			shmem_be_entry->num_nodes = (u32) num_nodes;
			shmem_be_entry->query_start_time = GetCurrentTimestamp();
			shmem_be_entry->query_id = get_query_id(queryDesc);

			/* Copy query text. */
			if (queryDesc->sourceText != NULL)
			{
				snprintf(query_text, PWH_QUERY_TEXT_LEN_GUC, "%s",
						 queryDesc->sourceText);
			}
			else
			{
				query_text[0] = '\0';
			}

			ereport(
				DEBUG1,
				(errmsg("PWH: ExecutorStart complete"),
				 errdetail("PID=%d query_id=%lu num_nodes=%lu query='%.100s'",
						   MyProcPid, (unsigned long) shmem_be_entry->query_id,
						   (unsigned long) num_nodes, query_text)));

			/* We're set. Store QueryDesc for signal handler. */
			pwh_set_current_query_desc(queryDesc);
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(old_context);

		/* Clear our state to avoid dangling references. */
		pwh_set_current_query_desc(NULL);

		/* Log the error but don't propagate to user query. */
		EmitErrorReport();
		FlushErrorState();

		ereport(LOG, (errmsg("PWH: Metric collection failed in ExecutorStart"),
					  errdetail("PID %d - query will continue without metrics",
								MyProcPid)));
	}
	PG_END_TRY();
}

static void
query_end_hook(QueryDesc *queryDesc)
{
	ereport(
		DEBUG2,
		(errmsg("PWH: ExecutorEnd called"),
		 errdetail(
			 "PID=%d signal_stats: calls=%lu success=%lu no_qd=%lu shm_null=%lu no_slot=%lu",
			 MyProcPid, (unsigned long) pwh_get_signal_handler_call_count(),
			 (unsigned long) pwh_get_signal_handler_success_count(),
			 (unsigned long) pwh_get_signal_handler_no_querydesc(),
			 (unsigned long) pwh_get_signal_handler_shmem_null(),
			 (unsigned long) pwh_get_signal_handler_no_slot())));

	if (likely(PWH_IS_ENABLED_GUC))
	{
		MemoryContext old_context = CurrentMemoryContext;

		PG_TRY();
		{
			/* Get our backend entry. */
			PwhSharedMemoryBackendEntry *shmem_be_entry =
				pwh_get_my_backend_entry();
			if (likely(shmem_be_entry != NULL &&
					   shmem_be_entry->backend_pid != 0))
			{
				PwhNodeMetrics *metrics =
					pwh_get_backend_entry_metrics(shmem_be_entry);

				/* Capture final instrumentation. */
				pwh_walk_plan_instrumentation(queryDesc->planstate, metrics,
											  PWH_MAX_NODES_PER_QUERY_GUC);

				ereport(DEBUG1,
						(errmsg("PWH: Completed query tracking for PID %d",
								MyProcPid),
						 errdetail(
							 "Generation: %lu",
							 (unsigned long) shmem_be_entry->poll_generation)));

				/* Clear entire slot to prevent cross-contamination. */
				MemSet(shmem_be_entry, 0, pwh_get_backend_entry_stride());
				PWH_MEMORY_BARRIER();
			}

			/* Clear QueryDesc pointer. */
			pwh_set_current_query_desc(NULL);
		}
		PG_CATCH();
		{
			MemoryContextSwitchTo(old_context);

			/* Clear our state to avoid dangling references. */
			pwh_set_current_query_desc(NULL);

			/* Log the error but don't propagate to user query. */
			EmitErrorReport();
			FlushErrorState();

			ereport(LOG,
					(errmsg("PWH: Metric collection failed in ExecutorEnd"),
					 errdetail("PID %d - query completed without final metrics",
							   MyProcPid)));
		}
		PG_END_TRY();
	}

	if (PREV_QUERY_FINISH_HOOK)
	{
		PREV_QUERY_FINISH_HOOK(queryDesc);
	}
	else
	{
		standard_ExecutorEnd(queryDesc);
	}
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

		TupleDesc td = pwh_create_v1_status_tupdesc();
		PWH_TUPLE_DESC_FINALIZE(td);
		funcctx->tuple_desc = BlessTupleDesc(td);

		/* Send SIGUSR2 to all active backends to refresh metrics. */
		u32 n_signaled = pwh_request_backend_metrics();

		ereport(DEBUG1, (errmsg("PWH: v1_status() called"),
						 errdetail("Signaled %u backends", n_signaled)));

		/* Wait for backends to refresh metrics. */
		pg_usleep(PWH_SIGNAL_TIMEOUT_MS_GUC * 1000L);

		/* XXX do it via static u64? */
		u32 *state = (u32 *) palloc(2 * sizeof(u32));
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
		if (shmem_be_entry != NULL && shmem_be_entry->backend_pid != 0 &&
			node_idx < shmem_be_entry->num_nodes)
		{
			PwhNodeMetrics *metrics =
				pwh_get_backend_entry_metrics(shmem_be_entry);
			PwhNodeMetrics *node = &metrics[node_idx];

			double total_query_time = 0.0;
			for (u32 j = 0; j < shmem_be_entry->num_nodes; j++)
			{
				total_query_time += metrics[j].execution.total_time_us;
			}

			Datum values[PWH_V1_STATUS_TUPLE_COUNT];
			bool  nulls[PWH_V1_STATUS_TUPLE_COUNT];

			pwh_fill_v1_status_tuple(values, nulls, shmem_be_entry, node,
									 total_query_time);

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
