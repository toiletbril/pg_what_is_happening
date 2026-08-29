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

/*
 * Main file of the extension. Ties up all initialization and exports workhorse
 * functions. OpenMetrics exporter is registered only if BG worker has been
 * compiled in and is located in bg_worker.c/h.
 *
 * Shared memory ownership changes use an exclusive lock. Metric writers use a
 * sequence counter, and readers copy consistent snapshots before formatting
 * or returning data.
 */

#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "common.h"
#include "compatibility.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "gucs.h"
#include "mb/pg_wchar.h"
#include "metrics.h"
#include "miscadmin.h"
#include "plan_tree_walker.h"
#include "shared_memory.h"
#include "signal_handler.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "utils/acl.h"
#include "utils/timestamp.h"

/*
 * XXX should we store a lock per backend instead of one global search lock?
 * Will that approach affect current best-effort metric collection?
 */

#ifdef WITH_BGWORKER
#include "bg_worker.h"
#endif

PG_MODULE_MAGIC;

/* Previous hooks. */
static ExecutorStart_hook_type PREV_QUERY_START_HOOK = NULL;
static ExecutorEnd_hook_type   PREV_QUERY_FINISH_HOOK = NULL;

void _PG_init(void);
void _PG_fini(void);

static void query_start_hook(QueryDesc *queryDesc, i32 eflags);
static void query_end_hook(QueryDesc *queryDesc);
static void query_cleanup_callback(XactEvent event, void *arg);
static void backend_exit_callback(int code, Datum arg);
static u64	get_query_id(const QueryDesc *qd);

PWH_SHMEM_REQUEST_HOOK_DECL;

static volatile bool WAS_BACKEND_INITIALIZED = false;

typedef struct
{
	PwhMetricsSnapshot *snapshot;
	u32					entry_index;
	u32					node_index;
} PwhStatusState;

static bool can_view_query_text(Oid owner_oid);

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
	pwh_calculate_shared_memory_layout();
	PWH_SHMEM_REQUEST_IN_STARTUP_HOOK();

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
	 * This functions fires either on ABORT or COMMIT. Only clean up on abort.
	 */
	if (!PWH_IS_ABORT_EVENT(event))
		return;

	/* Clear QueryDesc pointer to prevent dangling reference. */
	pwh_set_current_query_desc(NULL);
	pwh_set_signal_metrics(NULL, NULL, 0);
	pwh_release_my_backend_entry();

	ereport(DEBUG1, (errmsg("PWH: Cleaned up query state on abort for PID %d",
							MyProcPid)));
}

static void
backend_exit_callback(int code, Datum arg)
{
	unused(code);
	unused(arg);
	if (PWH_SHMEM == NULL)
		return;

	pwh_set_current_query_desc(NULL);
	pwh_set_signal_metrics(NULL, NULL, 0);

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
					(errmsg("PWH: Released backend entry %llu for PID %u",
							(unsigned long long) i, MyProcPid)));
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
	bool should_track = PWH_GUC_IS_ENABLED &&
						pwh_get_current_query_desc() == NULL &&
						queryDesc->plannedstmt != NULL &&
						queryDesc->plannedstmt->planTree != NULL &&
						queryDesc->plannedstmt->planTree->total_cost >=
							PWH_GUC_MIN_COST_TO_TRACK;

	ereport(DEBUG2,
			(errmsg("PWH: ExecutorStart hook called"),
			 errdetail("PID=%d enabled=%d instrument_options=%d eflags=%d",
					   MyProcPid, PWH_GUC_IS_ENABLED,
					   queryDesc->instrument_options, eflags)));

	if (should_track)
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

	if (!should_track)
		return;

	/* Okay, we're tracking this query. */

	if (unlikely(!WAS_BACKEND_INITIALIZED))
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
			PwhNodeInstrumentation **instrumentation = palloc0(
				sizeof(PwhNodeInstrumentation *) * PWH_GUC_MAX_NODES_PER_QUERY);

			/* Set initial backend state and prepare for metric collection. */
			sig_atomic_t base_sequence;
			if (!pwh_begin_metrics_write(be, &base_sequence))
				ereport(ERROR, (errmsg("PWH: Backend entry is being written")));
			u64 num_nodes = pwh_remember_planstate_tree_as_metric_structure(
				queryDesc->planstate, metrics, instrumentation,
				PWH_GUC_MAX_NODES_PER_QUERY);

			ereport(DEBUG1, (errmsg("PWH: Tracking query with %llu nodes",
									(unsigned long long) num_nodes),
							 errdetail("PID %d", MyProcPid)));

			be->count_of_metrics = (u32) num_nodes;
			be->query_start_time = GetCurrentTimestamp();
			be->query_id = get_query_id(queryDesc);

			/* Copy query text. */
			if (queryDesc->sourceText != NULL)
			{
				i32 source_len = strlen(queryDesc->sourceText);
				i32 copy_len = pg_mbcliplen(queryDesc->sourceText, source_len,
											PWH_GUC_MAX_QUERY_TEXT_LEN - 1);
				memcpy(query_text, queryDesc->sourceText, copy_len);
				query_text[copy_len] = '\0';
			}
			else
			{
				query_text[0] = '\0';
			}

			ereport(
				DEBUG1,
				(errmsg("PWH: ExecutorStart complete"),
				 errdetail("PID=%d query_id=%llu num_nodes=%llu query='%.100s'",
						   MyProcPid, (unsigned long long) be->query_id,
						   (unsigned long long) num_nodes, query_text)));

			pwh_end_metrics_write(be, base_sequence);

			/* Store the precomputed instrumentation map for the handler. */
			pwh_set_current_query_desc(queryDesc);
			pwh_set_signal_metrics(be, instrumentation, (u32) num_nodes);
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(old_context);

		/* Clear our state to avoid dangling references. */
		pwh_set_current_query_desc(NULL);
		pwh_set_signal_metrics(NULL, NULL, 0);
		pwh_release_my_backend_entry();

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
			 "PID=%d signal_stats: calls=%llu success=%llu no_qd=%llu shm_null=%llu no_slot=%llu",
			 MyProcPid,
			 (unsigned long long) pwh_get_signal_handler_call_count(),
			 (unsigned long long) pwh_get_signal_handler_success_count(),
			 (unsigned long long) pwh_get_signal_handler_no_querydesc(),
			 (unsigned long long) pwh_get_signal_handler_shmem_null(),
			 (unsigned long long) pwh_get_signal_handler_no_slot())));

	if (WAS_BACKEND_INITIALIZED && pwh_get_current_query_desc() == queryDesc)
	{
		MemoryContext old_context = CurrentMemoryContext;

		PG_TRY();
		{
			PwhSharedMemoryBackendEntry *be = pwh_get_my_backend_entry();

			if (likely(be != NULL))
			{
				if (likely(pwh_is_backend_entry_active(be)))
				{
					PwhNodeMetrics *metrics = pwh_get_backend_entry_metrics(be);

					/* Capture final instrumentation. */
					sig_atomic_t base_sequence;
					if (pwh_begin_metrics_write(be, &base_sequence))
					{
						pwh_collect_planstate_metrics(
							queryDesc->planstate, metrics,
							PWH_GUC_MAX_NODES_PER_QUERY);
						pwh_end_metrics_write(be, base_sequence);
					}

					ereport(DEBUG1,
							(errmsg("PWH: Completed query tracking for PID %d",
									MyProcPid),
							 errdetail("Generation: %d",
									   (int) be->poll_generation)));
				}
			}

			/* Clear state even if an orphan cleanup already released the slot.
			 */
			pwh_set_current_query_desc(NULL);
			pwh_set_signal_metrics(NULL, NULL, 0);
			pwh_release_my_backend_entry();
		}
		PG_CATCH();
		{
			MemoryContextSwitchTo(old_context);

			/* Clear our state to avoid dangling references. */
			pwh_set_current_query_desc(NULL);
			pwh_set_signal_metrics(NULL, NULL, 0);
			pwh_release_my_backend_entry();

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

static bool
can_view_query_text(Oid owner_oid)
{
	Oid current_user = GetUserId();
	if (superuser() || has_privs_of_role(current_user, owner_oid))
		return true;

#if PG_VERSION_NUM >= 100000
	Oid stats_role = get_role_oid("pg_read_all_stats", true);
	if (OidIsValid(stats_role) && has_privs_of_role(current_user, stats_role))
		return true;
#endif

	return false;
}

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

		pwh_refresh_metrics();
		PwhStatusState *state = palloc0(sizeof(PwhStatusState));
		state->snapshot = pwh_take_metrics_snapshot();
		funcctx->user_fctx = state;

		MemoryContextSwitchTo(oldcontext);
	}

	FuncCallContext *funcctx = SRF_PERCALL_SETUP();

	PwhStatusState *state = (PwhStatusState *) funcctx->user_fctx;

	while (state->entry_index < state->snapshot->count)
	{
		PwhSnapshotEntry *entry = &state->snapshot->entries[state->entry_index];
		if (state->node_index < entry->count_of_metrics)
		{
			PwhNodeMetrics *node = &entry->metrics[state->node_index];
			const char	   *query_text = can_view_query_text(entry->owner_oid)
											 ? entry->query_text
											 : "<insufficient privilege>";

			Datum values[PWH_V1_STATUS_TUPLE_COUNT];
			bool  nulls[PWH_V1_STATUS_TUPLE_COUNT];

			pwh_fill_v1_status_tuple(values, nulls, entry->backend_pid,
									 entry->query_id, query_text, node,
									 entry->total_query_time);

			HeapTuple tuple =
				heap_form_tuple(funcctx->tuple_desc, values, nulls);

			/* Advance to next node. */
			state->node_index++;

			SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
		}

		/* Move to next slot. */
		state->entry_index++;
		state->node_index = 0;
	}

	SRF_RETURN_DONE(funcctx);
}
