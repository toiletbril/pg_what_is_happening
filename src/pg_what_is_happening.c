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
#include "common.h"
#include "compatibility.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "gucs.h"
#include "metrics.h"
#include "miscadmin.h"
#include "plan_tree_walker.h"
#include "shared_memory.h"
#include "signal_handler.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "utils/timestamp.h"

#ifdef WITH_BGWORKER
#include "bg_worker.h"
#endif

PG_MODULE_MAGIC;

/* Previous hooks. */
static ExecutorStart_hook_type PREV_QUERY_START_HOOK = NULL;
static ExecutorEnd_hook_type   PREV_QUERY_FINISH_HOOK = NULL;
extern shmem_startup_hook_type PREV_SHMEM_STARTUP_HOOK;

void _PG_init(void);
void _PG_fini(void);

static void query_start_hook(QueryDesc *queryDesc, i32 eflags);
static void query_end_hook(QueryDesc *queryDesc);
static void query_cleanup_callback(XactEvent event, void *arg);
static void backend_exit_callback(int code, Datum arg);
static u64	get_query_id(const QueryDesc *qd);

PWH_SHMEM_REQUEST_HOOK_DECL;

static volatile bool WAS_BACKEND_INITIALIZED = false;

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
	pwh_define_gucs();

	/* Install hooks. */
	PWH_INSTALL_SHMEM_REQUEST_HOOK();

	PREV_SHMEM_STARTUP_HOOK = shmem_startup_hook;
	shmem_startup_hook = pwh_shared_memory_startup_hook;

	PREV_QUERY_START_HOOK = ExecutorStart_hook;
	ExecutorStart_hook = query_start_hook;

	PREV_QUERY_FINISH_HOOK = ExecutorEnd_hook;
	ExecutorEnd_hook = query_end_hook;

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

static u64
get_query_id(const QueryDesc *qd)
{
	u64 id = PWH_GET_QUERY_ID(qd->plannedstmt);

	/*
	 * Fallback to hash if queryId is 0 (not populated without
	 * pg_stat_statements).
	 */
	if (id == 0)
	{
		id = pwh_compute_query_id(qd);
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

	/*
	 * Only clean up on abort - commit path goes through ExecutorEnd normally.
	 */
	if (!PWH_IS_ABORT_EVENT(event))
		return;

	/* Clear QueryDesc pointer to prevent dangling reference. */
	pwh_set_current_query_desc(NULL);

	if (!PWH_GUC_IS_ENABLED)
		return;

	pwh_release_my_backend_entry();

	ereport(DEBUG1, (errmsg("PWH: Cleaned up query state on abort for PID %d",
							MyProcPid)));
}

static void
backend_exit_callback(int code, Datum arg)
{
	unused(code);
	unused(arg);

	PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_EXCLUSIVE);

	for (u64 i = 0; i < (u64) PWH_GUC_MAX_TRACKED_QUERIES; i++)
	{
		PwhSharedMemoryBackendEntry *be = PWH_GET_BACKEND_ENTRY_UNSAFE(i);
		Assert(be != NULL);

		if (be->backend_pid == MyProcPid)
		{
			pwh_release_backend_entry_unlocked(be);
			PWH_MEMORY_BARRIER();
			ereport(DEBUG2,
					(errmsg("PWH: Released backend entry %lu for PID %u", i,
							MyProcPid)));
			break;
		}
	}

	PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);

	WAS_BACKEND_INITIALIZED = false;
}

static void
initialize_state_once_per_backend(void)
{
	/* Query backend state on error. */
	RegisterXactCallback(query_cleanup_callback, NULL);
	before_shmem_exit(backend_exit_callback, (Datum) 0);
	/* Report metrics when signaled. */
	pwh_install_signal_handler();
	/* Communicate with BG worker. */
	PWH_SHMEM = pwh_get_shared_memory_ptr();

	WAS_BACKEND_INITIALIZED = true;
}

static void
query_start_hook(QueryDesc *queryDesc, i32 eflags)
{
	ereport(DEBUG2,
			(errmsg("PWH: ExecutorStart hook called"),
			 errdetail("PID=%d enabled=%d instrument_options=%d eflags=%d",
					   MyProcPid, PWH_GUC_IS_ENABLED,
					   queryDesc->instrument_options, eflags)));

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

	ereport(DEBUG1,
			(errmsg("PWH: After ExecutorStart, checking instrumentation"),
			 errdetail("planstate=%p planstate->instrument=%p",
					   (void *) queryDesc->planstate,
					   queryDesc->planstate
						   ? (void *) queryDesc->planstate->instrument
						   : NULL)));

	if (unlikely(!PWH_GUC_IS_ENABLED))
	{
		return;
	}

	/* Does this query satisfy the minimum cost constraint? */
	if (queryDesc->plannedstmt->planTree->total_cost <
		PWH_GUC_MIN_COST_TO_TRACK)
	{
		return;
	}

	/* Okay, we're tracking this query. */

	if (!WAS_BACKEND_INITIALIZED)
	{
		initialize_state_once_per_backend();
	}

	MemoryContext old_context = CurrentMemoryContext;

	PG_TRY();
	{
		PwhSharedMemoryBackendEntry *be = pwh_get_or_create_my_backend_entry();

		if (unlikely(be == NULL))
		{
			ereport(LOG, (errmsg("PWH: Could not allocate backend entry"),
						  errdetail("PID %d exhausted all slots", MyProcPid)));
		}
		else
		{
			PwhNodeMetrics *metrics = pwh_get_backend_entry_metrics(be);
			char		   *query_text = pwh_get_backend_entry_query_text(be);

			/* Set initial backend state and prepare for metric collection. */
			u64 num_nodes = pwh_remember_planstate_tree_as_metric_structure(
				queryDesc->planstate, metrics, PWH_GUC_MAX_NODES_PER_QUERY, -1);

			ereport(DEBUG1, (errmsg("PWH: Tracking query with %lu nodes",
									(unsigned long) num_nodes),
							 errdetail("PID %d", MyProcPid)));

			be->count_of_metrics = (u32) num_nodes;
			be->query_start_time = GetCurrentTimestamp();
			be->query_id = get_query_id(queryDesc);

			/* Copy query text. */
			if (queryDesc->sourceText != NULL)
			{
				snprintf(query_text, PWH_GUC_MAX_QUERY_TEXT_LEN, "%s",
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
						   MyProcPid, (unsigned long) be->query_id,
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
		backend_exit_callback(-1, 0);

		/* Log the error but don't propagate to user query. */
		EmitErrorReport();
		FlushErrorState();

		ereport(LOG,
				(errmsg("PWH: Metric collection failed for PID %d", MyProcPid),
				 errdetail("Query will continue without metrics")));
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

	if (likely(PWH_GUC_IS_ENABLED && WAS_BACKEND_INITIALIZED))
	{
		MemoryContext old_context = CurrentMemoryContext;

		PG_TRY();
		{
			/* Get our backend entry. */
			PwhSharedMemoryBackendEntry *be = pwh_get_my_backend_entry();

			if (be != NULL)
			{
				PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_EXCLUSIVE);

				if (likely(pwh_is_backend_entry_active(be)))
				{
					PwhNodeMetrics *metrics = pwh_get_backend_entry_metrics(be);

					/* Capture final instrumentation. */
					pwh_collect_planstate_metrics(queryDesc->planstate, metrics,
												  PWH_GUC_MAX_NODES_PER_QUERY);

					ereport(DEBUG1,
							(errmsg("PWH: Completed query tracking for PID %d",
									MyProcPid),
							 errdetail("Generation: %lu",
									   (unsigned long) be->poll_generation)));

					pwh_release_backend_entry_unlocked(be);
				}

				PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);


				/* Clear QueryDesc pointer. */
				pwh_set_current_query_desc(NULL);
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

			PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);

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

		/* Lock the shared memory until we are done reading. */
		PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_SHARED);

		/* Send SIGUSR2 to all active backends to refresh metrics. */
		u32 n_signaled = pwh_request_backend_metrics_unlocked();

		/* Wait for backends to refresh metrics. */
		pg_usleep(PWH_GUC_SIGNAL_TIMEOUT_MS * 1000L);

		PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);

		ereport(DEBUG1, (errmsg("PWH: v1_status() starting to read metrics"),
						 errdetail("Signaled %u backends", n_signaled)));

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
	while (slot_idx < (u32) PWH_GUC_MAX_TRACKED_QUERIES)
	{
		PwhSharedMemoryBackendEntry *be = pwh_get_backend_entry(slot_idx);

		if (pwh_is_backend_entry_active(be) && node_idx < be->count_of_metrics)
		{
			PwhNodeMetrics *metrics = pwh_get_backend_entry_metrics(be);
			PwhNodeMetrics *node = &metrics[node_idx];

			if (node_idx == 0)
			{
				ereport(
					DEBUG2,
					(errmsg("PWH: v1_status() reading backend slot %u",
							slot_idx),
					 errdetail("pid=%d query_id=%lu num_nodes=%u",
							   be->backend_pid, (unsigned long) be->query_id,
							   be->count_of_metrics)));
			}

			double total_query_time = 0.0;
			for (u32 j = 0; j < be->count_of_metrics; j++)
			{
				total_query_time += metrics[j].execution.total_time_us;
			}

			ereport(
				DEBUG2,
				(errmsg("PWH: v1_status() reading node %u", node_idx),
				 errdetail(
					 "total_time_us=%.0f tuples_returned=%.0f cache_hits=%ld",
					 node->execution.total_time_us,
					 node->execution.tuples_returned,
					 node->buffer_usage.cache_hits)));

			Datum values[PWH_V1_STATUS_TUPLE_COUNT];
			bool  nulls[PWH_V1_STATUS_TUPLE_COUNT];

			pwh_fill_v1_status_tuple(values, nulls, be, node, total_query_time);

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
