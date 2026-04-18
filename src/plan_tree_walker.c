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
	PwhNodeMetrics *metrics;
	u64				max_nodes;
	u64			   *node_counter;
} WalkerContext;

/* Returns assigned node id, or -1 to stop traversal. */
typedef i32 (*PwhNodeVisitorFn)(PlanState *planstate, i32 parent_id,
								void *context);

static i32 topology_visitor(PlanState *planstate, i32 parent_id, void *context);
static i32 instrumentation_visitor(PlanState *planstate, i32 parent_id,
								   void *context);
static bool walk_planstate_recursive(PlanState *planstate, i32 parent_id,
									 PwhNodeVisitorFn visit_fn, void *ctx);

static bool
walk_planstate_recursive(PlanState *planstate, i32 parent_id,
						 PwhNodeVisitorFn visit_fn, void *ctx)
{
	check_stack_depth();

	if (planstate == NULL || planstate->plan == NULL)
		return true;

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
pwh_remember_planstate_tree_as_metric_structure(PlanState	   *planstate,
												PwhNodeMetrics *metrics,
												u64				max_nodes)
{
	u64 node_counter = 0;

	if (unlikely(planstate == NULL || metrics == NULL))
		return 0;

	WalkerContext ctx = {
		.metrics = metrics,
		.max_nodes = max_nodes,
		.node_counter = &node_counter,
	};

	walk_planstate_recursive(planstate, -1, topology_visitor, &ctx);

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

	return id;
}

/*
 * Walk plan tree and read Instrumentation data.
 * Must match the same traversal order as topology walk.
 */
void
pwh_collect_planstate_metrics(PlanState *planstate, PwhNodeMetrics *metrics,
							  u64 max_nodes)
{
	u64 node_counter = 0;

	if (unlikely(planstate == NULL || metrics == NULL))
		return;

	WalkerContext ctx = {
		.metrics = metrics,
		.max_nodes = max_nodes,
		.node_counter = &node_counter,
	};

	walk_planstate_recursive(planstate, -1, instrumentation_visitor, &ctx);
}

static i32
instrumentation_visitor(PlanState *planstate, i32 parent_id, void *context)
{
	WalkerContext *ctx = (WalkerContext *) context;
	(void) parent_id;

	if (*ctx->node_counter >= ctx->max_nodes)
		return -1;

	i32				 current_id = (i32) (*ctx->node_counter)++;
	Instrumentation *instr = planstate->instrument;

	/* Validate node magic before writing. */
	if (!pwh_validate_node_magic(&ctx->metrics[current_id], (u32) current_id))
		return -1;

	if (likely(instr != NULL))
	{
		ereport(DEBUG2,
				(errmsg("PWH: Reading instrumentation for node %d", current_id),
				 errdetail(
					 "ntuples=%.0f nloops=%.0f total_time=%.6f cache_hits=%ld",
					 instr->ntuples, instr->nloops,
					 PWH_INSTR_TIME_MAYBE_GET_DOUBLE(instr->total),
					 instr->bufusage.shared_blks_hit)));

		ctx->metrics[current_id].execution.tuples_returned =
			instr->ntuples + instr->tuplecount;
		ctx->metrics[current_id].execution.loops_executed =
			instr->nloops + (instr->running ? 1.0 : 0.0);
		ctx->metrics[current_id].execution.startup_time_us =
			(PWH_INSTR_TIME_MAYBE_GET_DOUBLE(instr->startup) +
			 PWH_INSTR_TIME_MAYBE_GET_DOUBLE(instr->firsttuple)) *
			1000000.0;
		ctx->metrics[current_id].execution.total_time_us =
			(PWH_INSTR_TIME_MAYBE_GET_DOUBLE(instr->total) +
			 INSTR_TIME_GET_DOUBLE(instr->counter)) *
			1000000.0;
		ctx->metrics[current_id].execution.rows_filtered_by_joins =
			instr->nfiltered1;
		ctx->metrics[current_id].execution.rows_filtered_by_expressions =
			instr->nfiltered2;

		PWH_COPY_BUFUSAGE(ctx->metrics, instr, current_id);
	}
	else
	{
		ereport(LOG,
				(errmsg("PWH: Node %d has NULL instrumentation", current_id),
				 errdetail("planstate=%p", (void *) planstate)));
	}

	return current_id;
}
