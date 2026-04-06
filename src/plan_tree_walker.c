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
	i32				parent_id; /* -1 means no parent. */
} TopologyContext;

typedef struct
{
	PwhNodeMetrics *metrics;
	u64				max_nodes;
	u64			   *node_counter;
} InstrumentationContext;


typedef bool (*PwhNodeVisitorFn)(PlanState *planstate, void *context);

static bool topology_visitor(PlanState *planstate, void *context);
static void walk_topology_with_parent(PlanState *planstate, i32 parent_id,
									  TopologyContext *ctx);

static bool instrumentation_visitor(PlanState *planstate, void *context);
static void walk_planstate_tree(PlanState *planstate, PwhNodeVisitorFn visitor,
								void *context);

static void
walk_planstate_tree(PlanState *planstate, PwhNodeVisitorFn visitor,
					void *context)
{
	if (planstate == NULL)
		return;

	pwh_walk_planstate_recursive(planstate, visitor, context);
}

bool
pwh_walk_planstate_recursive(PlanState *planstate, PwhNodeVisitorFn visitor,
							 void *context)
{
	check_stack_depth();

	if (planstate == NULL)
	{
		return true;
	}

	if (!visitor(planstate, context))
	{
		return false;
	}

	/* Standard children: left, right, subPlan. */
	if (planstate->lefttree != NULL)
	{
		if (!pwh_walk_planstate_recursive(planstate->lefttree, visitor,
										  context))
		{
			return false;
		}
	}
	if (planstate->righttree != NULL)
	{
		if (!pwh_walk_planstate_recursive(planstate->righttree, visitor,
										  context))
		{
			return false;
		}
	}
	if (planstate->subPlan != NULL)
	{
		ListCell *lc;
		foreach (lc, planstate->subPlan)
		{
			SubPlanState *subplanstate = (SubPlanState *) lfirst(lc);
			if (!pwh_walk_planstate_recursive(subplanstate->planstate, visitor,
											  context))
			{
				return false;
			}
		}
	}

	/* Version-specific or non-standard children. */
	return pwh_walk_planstate_children(planstate, visitor, context);
}

/*
 * Returns total number of nodes found.
 */
u64
pwh_remember_planstate_tree_as_metric_structure(PlanState	   *planstate,
												PwhNodeMetrics *metrics,
												u64 max_nodes, i32 parent_id)
{
	u64 node_counter = 0;

	if (unlikely(planstate == NULL || metrics == NULL))
		return 0;

	TopologyContext ctx = {
		.metrics = metrics,
		.max_nodes = max_nodes,
		.node_counter = &node_counter,
	};

	walk_topology_with_parent(planstate, parent_id, &ctx);

	return node_counter;
}

static bool
topology_visitor(PlanState *planstate, void *context)
{
	TopologyContext *ctx = (TopologyContext *) context;

	if (planstate == NULL || planstate->plan == NULL)
		return false;

	if (*ctx->node_counter >= ctx->max_nodes)
		return false;

	i32 id = (*ctx->node_counter)++;

	ctx->metrics[id].node_id = id;
	ctx->metrics[id].parent_node_id = ctx->parent_id;
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

	return true;
}

static bool
topology_recurse_visitor(PlanState *planstate, void *context)
{
	TopologyContext *ctx = (TopologyContext *) context;
	walk_topology_with_parent(planstate, ctx->parent_id, ctx);
	return false;
}

static void
walk_topology_with_parent(PlanState *planstate, i32 parent_id,
						  TopologyContext *ctx)
{
	check_stack_depth();

	if (planstate == NULL || *ctx->node_counter >= ctx->max_nodes)
		return;

	ctx->parent_id = parent_id;
	if (!topology_visitor(planstate, ctx))
		return;

	i32 current_id = (*ctx->node_counter) - 1;
	ctx->parent_id = current_id;

	if (planstate->lefttree != NULL)
		walk_topology_with_parent(planstate->lefttree, current_id, ctx);
	if (planstate->righttree != NULL)
		walk_topology_with_parent(planstate->righttree, current_id, ctx);

	if (planstate->subPlan != NULL)
	{
		ListCell *lc;
		foreach (lc, planstate->subPlan)
		{
			SubPlanState *subplanstate = (SubPlanState *) lfirst(lc);
			walk_topology_with_parent(subplanstate->planstate, current_id, ctx);
		}
	}

	pwh_walk_planstate_children(planstate, topology_recurse_visitor, ctx);
}

/*
 * Walk plan tree and read Instrumentation data
 * Must match the same traversal order as topology walk
 */
void
pwh_collect_planstate_metrics(PlanState *planstate, PwhNodeMetrics *metrics,
							  u64 max_nodes)
{
	u64 node_counter = 0;

	if (unlikely(planstate == NULL || metrics == NULL))
	{
		return;
	}

	InstrumentationContext ctx = {
		.metrics = metrics,
		.max_nodes = max_nodes,
		.node_counter = &node_counter,
	};

	pwh_walk_planstate_recursive(planstate, instrumentation_visitor, &ctx);
}

static bool
instrumentation_visitor(PlanState *planstate, void *context)
{
	InstrumentationContext *ctx = (InstrumentationContext *) context;

	if (planstate == NULL)
		return false;

	if (*ctx->node_counter >= ctx->max_nodes)
		return false;

	u64				 current_id = (*ctx->node_counter)++;
	Instrumentation *instr = planstate->instrument;

	/* Validate node magic before writing. */
	if (!pwh_validate_node_magic(&ctx->metrics[current_id], (u32) current_id))
		return false;

	if (likely(instr != NULL))
	{
		ereport(
			DEBUG2,
			(errmsg("PWH: Reading instrumentation for node %lu", current_id),
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
				(errmsg("PWH: Node %lu has NULL instrumentation", current_id),
				 errdetail("planstate=%p", (void *) planstate)));
	}

	return true;
}
