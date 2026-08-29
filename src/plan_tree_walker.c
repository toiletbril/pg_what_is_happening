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
 * Rewritten, version-agnostic (via compatibility header) planstate walker and
 * friend routines used for metric collection.
 */

#include "postgres.h"

#include "plan_tree_walker.h"

#include "common.h"
#include "compatibility.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "portability/instr_time.h"
#include "shared_memory.h"

typedef struct
{
	PwhNodeMetrics			*metrics;
	PwhNodeInstrumentation **instrumentation;
	u64						 max_nodes;
	u64						*node_counter;
	PlanState			   **visited;
	u64						 visited_count;
} WalkerContext;

/* Returns assigned node id, or -1 to stop traversal. */
typedef i32 (*PwhNodeVisitorFn)(PlanState *planstate, i32 parent_id,
								void *context);

static i32 topology_visitor(PlanState *planstate, i32 parent_id, void *context);
static bool walk_planstate_recursive(PlanState *planstate, i32 parent_id,
									 PwhNodeVisitorFn visit_fn, void *ctx);

static bool
walk_planstate_recursive(PlanState *planstate, i32 parent_id,
						 PwhNodeVisitorFn visit_fn, void *ctx)
{
	check_stack_depth();

	if (planstate == NULL || planstate->plan == NULL)
		return true;

	WalkerContext *walker = (WalkerContext *) ctx;
	for (u64 i = 0; i < walker->visited_count; i++)
		if (walker->visited[i] == planstate)
			return true;
	if (walker->visited_count >= walker->max_nodes)
		return false;
	walker->visited[walker->visited_count++] = planstate;

	i32 current_id = visit_fn(planstate, parent_id, ctx);
	if (current_id < 0)
	{
		return false;
	}

	/* Standard children. */
	if (planstate->lefttree != NULL)
	{
		if (!walk_planstate_recursive(planstate->lefttree, current_id, visit_fn,
									  ctx))
		{
			return false;
		}
	}

	if (planstate->righttree != NULL)
	{
		if (!walk_planstate_recursive(planstate->righttree, current_id,
									  visit_fn, ctx))
		{
			return false;
		}
	}

	if (planstate->subPlan != NULL)
	{
		ListCell *lc;
		foreach (lc, planstate->subPlan)
		{
			SubPlanState *sp = (SubPlanState *) lfirst(lc);
			if (!walk_planstate_recursive(sp->planstate, current_id, visit_fn,
										  ctx))
				return false;
		}
	}

	if (planstate->initPlan != NULL)
	{
		ListCell *lc;
		foreach (lc, planstate->initPlan)
		{
			SubPlanState *sp = (SubPlanState *) lfirst(lc);
			if (!walk_planstate_recursive(sp->planstate, current_id, visit_fn,
										  ctx))
				return false;
		}
	}

	switch (nodeTag(planstate))
	{
		case T_AppendState:
		{
			AppendState *as = (AppendState *) planstate;
			for (u64 i = 0; i < (u64) as->as_nplans; i++)
				if (!walk_planstate_recursive(as->appendplans[i], current_id,
											  visit_fn, ctx))
					return false;
			break;
		}
		case T_CteScanState:
		{
			CteScanState *cs = (CteScanState *) planstate;
			if (cs->cteplanstate != NULL)
				if (!walk_planstate_recursive(cs->cteplanstate, current_id,
											  visit_fn, ctx))
					return false;
			break;
		}
#if PG_VERSION_NUM < 140000
		case T_ModifyTableState:
		{
			ModifyTableState *mt = (ModifyTableState *) planstate;
			for (u64 i = 0; i < (u64) mt->mt_nplans; i++)
				if (!walk_planstate_recursive(mt->mt_plans[i], current_id,
											  visit_fn, ctx))
					return false;
			break;
		}
#endif
		case T_MergeAppendState:
		{
			MergeAppendState *mas = (MergeAppendState *) planstate;
			for (u64 i = 0; i < (u64) mas->ms_nplans; i++)
				if (!walk_planstate_recursive(mas->mergeplans[i], current_id,
											  visit_fn, ctx))
					return false;
			break;
		}
		case T_BitmapAndState:
		{
			BitmapAndState *bas = (BitmapAndState *) planstate;
			for (u64 i = 0; i < (u64) bas->nplans; i++)
				if (!walk_planstate_recursive(bas->bitmapplans[i], current_id,
											  visit_fn, ctx))
					return false;
			break;
		}
		case T_BitmapOrState:
		{
			BitmapOrState *bos = (BitmapOrState *) planstate;
			for (u64 i = 0; i < (u64) bos->nplans; i++)
				if (!walk_planstate_recursive(bos->bitmapplans[i], current_id,
											  visit_fn, ctx))
					return false;
			break;
		}
		case T_SubqueryScanState:
		{
			SubqueryScanState *sqs = (SubqueryScanState *) planstate;
			if (sqs->subplan != NULL)
				if (!walk_planstate_recursive(sqs->subplan, current_id,
											  visit_fn, ctx))
					return false;
			break;
		}
#if PG_VERSION_NUM >= 90500
		case T_CustomScanState:
		{
			CustomScanState *css = (CustomScanState *) planstate;
			ListCell		*lc;
			foreach (lc, css->custom_ps)
				if (!walk_planstate_recursive((PlanState *) lfirst(lc),
											  current_id, visit_fn, ctx))
					return false;
			break;
		}
#endif
		default:
			break;
	}

	/* XXX version specific nodes. */
	return true;
}

/*
 * Returns total number of nodes found.
 */
u64
pwh_remember_planstate_tree_as_metric_structure(
	PlanState *planstate, PwhNodeMetrics *metrics,
	PwhNodeInstrumentation **instrumentation, u64 max_nodes)
{
	u64 node_counter = 0;

	if (unlikely(planstate == NULL || metrics == NULL))
		return 0;

	WalkerContext ctx = {
		.metrics = metrics,
		.instrumentation = instrumentation,
		.max_nodes = max_nodes,
		.node_counter = &node_counter,
		.visited = palloc0(sizeof(PlanState *) * max_nodes),
	};

	walk_planstate_recursive(planstate, -1, topology_visitor, &ctx);
	pfree(ctx.visited);

	return node_counter;
}

static i32
topology_visitor(PlanState *planstate, i32 parent_id, void *context)
{
	WalkerContext *ctx = (WalkerContext *) context;

	if (*ctx->node_counter >= ctx->max_nodes)
		return -1;

	i32 id = (i32) (*ctx->node_counter)++;

	ctx->metrics[id].node_id = id;
	ctx->metrics[id].parent_node_id = parent_id;
	ctx->metrics[id].tag = nodeTag(planstate->plan);

	ctx->metrics[id].execution.tuples_returned = 0;
	ctx->metrics[id].execution.startup_time_us = 0;
	ctx->metrics[id].execution.total_time_us = 0;
	ctx->metrics[id].execution.loops_executed = 0;
	ctx->metrics[id].execution.rows_filtered_by_joins = 0;
	ctx->metrics[id].execution.rows_filtered_by_expressions = 0;
	ctx->metrics[id].buffer_usage.cache_hits = 0;
	ctx->metrics[id].buffer_usage.cache_misses = 0;
	ctx->metrics[id].buffer_usage.local_cache_hits = 0;
	ctx->metrics[id].buffer_usage.local_cache_misses = 0;
	ctx->metrics[id].buffer_usage.spill_file_reads = 0;
	ctx->metrics[id].buffer_usage.spill_file_writes = 0;

	ctx->metrics[id].magic = PWH_NODE_MAGIC;
	if (ctx->instrumentation != NULL)
		ctx->instrumentation[id] = planstate->instrument;

	return id;
}

void
pwh_collect_instrumentation_metrics(PwhNodeInstrumentation **instrumentation,
									PwhNodeMetrics *metrics, u64 count)
{
	if (instrumentation == NULL || metrics == NULL)
		return;

	for (u64 i = 0; i < count; i++)
	{
		PwhNodeInstrumentation *instr = instrumentation[i];
		if (instr == NULL || metrics[i].magic != PWH_NODE_MAGIC)
			continue;

		metrics[i].execution.tuples_returned =
			instr->ntuples + instr->tuplecount;
		metrics[i].execution.loops_executed =
			instr->nloops + (instr->running ? 1.0 : 0.0);
		metrics[i].execution.startup_time_us =
			(PWH_INSTR_TIME_MAYBE_GET_DOUBLE(instr->startup) +
			 PWH_INSTR_TIME_MAYBE_GET_DOUBLE(instr->firsttuple)) *
			1000000.0;
		metrics[i].execution.total_time_us =
			(PWH_INSTR_TIME_MAYBE_GET_DOUBLE(PWH_INSTR_TOTAL(instr)) +
			 INSTR_TIME_GET_DOUBLE(instr->counter)) *
			1000000.0;
		metrics[i].execution.rows_filtered_by_joins = instr->nfiltered1;
		metrics[i].execution.rows_filtered_by_expressions = instr->nfiltered2;
		PWH_COPY_BUFUSAGE(metrics, instr, i);
	}
}
