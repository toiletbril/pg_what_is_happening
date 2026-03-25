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
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "portability/instr_time.h"
#include "shared_memory.h"

static bool pwh_walk_planstate_recursive(PlanState		 *planstate,
										 PwhNodeVisitorFn visitor,
										 void			 *context);
static i32	walk_topology_recursive(PlanState *planstate, PwhNode *metrics,
									i32 max_nodes, i32 parent_id,
									i32 *node_counter);
static void walk_instrumentation_recursive(PlanState *planstate,
										   PwhNode *metrics, i32 max_nodes,
										   i32 *node_counter);

/*
 * Generic planstate tree walker
 * Calls visitor on each node, then recursively walks standard and
 * version-specific children
 */
void
pwh_walk_planstate_tree(PlanState *planstate, PwhNodeVisitorFn visitor,
						void *context)
{
	if (planstate == NULL)
	{
		return;
	}
	pwh_walk_planstate_recursive(planstate, visitor, context);
}

static bool
pwh_walk_planstate_recursive(PlanState *planstate, PwhNodeVisitorFn visitor,
							 void *context)
{
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
			PlanState *subplan = (PlanState *) lfirst(lc);
			if (!pwh_walk_planstate_recursive(subplan, visitor, context))
			{
				return false;
			}
		}
	}

	/* Version-specific non-standard children. */
	return pwh_walk_planstate_children(planstate, visitor, context);
}

/*
 * Walk plan tree and populate topology information
 * Returns total number of nodes found
 */
i32
pwh_walk_plan_topology(PlanState *planstate, PwhNode *metrics, i32 max_nodes,
					   i32 parent_id)
{
	i32 node_counter = 0;

	if (unlikely(!planstate || !metrics))
	{
		return 0;
	}

	walk_topology_recursive(planstate, metrics, max_nodes, parent_id,
							&node_counter);
	return node_counter;
}

/*
 * Recursive helper for topology walking
 */
static i32
walk_topology_recursive(PlanState *planstate, PwhNode *metrics, i32 max_nodes,
						i32 parent_id, i32 *node_counter)
{
	if (!planstate || *node_counter >= max_nodes)
	{
		return -1;
	}

	i32 current_id = *node_counter;
	(*node_counter)++;

	NodeTag plan_tag = nodeTag(planstate->plan);
	metrics[current_id].node_id = current_id;
	metrics[current_id].parent_node_id = parent_id;
	metrics[current_id].tag = plan_tag;
	snprintf(metrics[current_id].node_type_name, PWH_NODE_TYPE_NAME_LEN, "%s",
			 pwh_node_type_to_string(plan_tag));

	metrics[current_id].execution.tuples_returned = 0;
	metrics[current_id].execution.startup_time_us = 0;
	metrics[current_id].execution.total_time_us = 0;
	metrics[current_id].execution.loops_executed = 0;
	metrics[current_id].buffer_usage.shared_hit = 0;
	metrics[current_id].buffer_usage.shared_read = 0;
	metrics[current_id].buffer_usage.local_hit = 0;
	metrics[current_id].buffer_usage.local_read = 0;
	metrics[current_id].buffer_usage.temp_read = 0;
	metrics[current_id].buffer_usage.temp_written = 0;

	/* Recurse to child nodes. */
	if (planstate->lefttree)
	{
		walk_topology_recursive(planstate->lefttree, metrics, max_nodes,
								current_id, node_counter);
	}
	if (planstate->righttree)
	{
		walk_topology_recursive(planstate->righttree, metrics, max_nodes,
								current_id, node_counter);
	}

	if (planstate->subPlan)
	{
		ListCell *lc;
		foreach (lc, planstate->subPlan)
		{
			PlanState *subplan = (PlanState *) lfirst(lc);
			walk_topology_recursive(subplan, metrics, max_nodes, current_id,
									node_counter);
		}
	}

	switch (nodeTag(planstate))
	{
		case T_AppendState:
		{
			AppendState *appendstate = (AppendState *) planstate;
			for (i32 i = 0; i < appendstate->as_nplans; i++)
			{
				walk_topology_recursive(appendstate->appendplans[i], metrics,
										max_nodes, current_id, node_counter);
			}
			break;
		}
		case T_MergeAppendState:
		{
			MergeAppendState *mergeappendstate = (MergeAppendState *) planstate;
			for (i32 i = 0; i < mergeappendstate->ms_nplans; i++)
			{
				walk_topology_recursive(mergeappendstate->mergeplans[i],
										metrics, max_nodes, current_id,
										node_counter);
			}
			break;
		}
		case T_BitmapAndState:
		{
			BitmapAndState *bitmapandstate = (BitmapAndState *) planstate;
			for (i32 i = 0; i < bitmapandstate->nplans; i++)
			{
				walk_topology_recursive(bitmapandstate->bitmapplans[i], metrics,
										max_nodes, current_id, node_counter);
			}
			break;
		}
		case T_BitmapOrState:
		{
			BitmapOrState *bitmaporstate = (BitmapOrState *) planstate;
			for (i32 i = 0; i < bitmaporstate->nplans; i++)
			{
				walk_topology_recursive(bitmaporstate->bitmapplans[i], metrics,
										max_nodes, current_id, node_counter);
			}
			break;
		}
		case T_SubqueryScanState:
		{
			SubqueryScanState *subqueryscan = (SubqueryScanState *) planstate;
			if (subqueryscan->subplan)
			{
				walk_topology_recursive(subqueryscan->subplan, metrics,
										max_nodes, current_id, node_counter);
			}
			break;
		}
		default:
			break;
	}

	return current_id;
}

/*
 * Walk plan tree and read Instrumentation data
 * Must match the same traversal order as topology walk
 */
void
pwh_walk_plan_instrumentation(PlanState *planstate, PwhNode *metrics,
							  i32 max_nodes)
{
	i32 node_counter = 0;

	if (unlikely(!planstate || !metrics))
	{
		return;
	}

	walk_instrumentation_recursive(planstate, metrics, max_nodes,
								   &node_counter);
}

/*
 * Recursive helper for instrumentation reading
 */
static void
walk_instrumentation_recursive(PlanState *planstate, PwhNode *metrics,
							   i32 max_nodes, i32 *node_counter)
{
	if (!planstate || *node_counter >= max_nodes)
	{
		return;
	}

	i32 current_id = *node_counter;
	(*node_counter)++;

	Instrumentation *instr = planstate->instrument;
	if (likely(instr))
	{
		metrics[current_id].execution.tuples_returned =
			instr->ntuples + instr->tuplecount;
		metrics[current_id].execution.loops_executed =
			instr->nloops + (instr->running ? 1.0 : 0.0);
		metrics[current_id].execution.startup_time_us =
			(instr->startup + instr->firsttuple) * 1000000.0;
		metrics[current_id].execution.total_time_us =
			(instr->total + INSTR_TIME_GET_DOUBLE(instr->counter)) * 1000000.0;

		PWH_COPY_BUFUSAGE(metrics, instr, current_id);
	}

	/* Recurse to child nodes (same order as topology walk). */
	if (planstate->lefttree)
	{
		walk_instrumentation_recursive(planstate->lefttree, metrics, max_nodes,
									   node_counter);
	}
	if (planstate->righttree)
	{
		walk_instrumentation_recursive(planstate->righttree, metrics, max_nodes,
									   node_counter);
	}

	if (planstate->subPlan)
	{
		ListCell *lc;
		foreach (lc, planstate->subPlan)
		{
			PlanState *subplan = (PlanState *) lfirst(lc);
			walk_instrumentation_recursive(subplan, metrics, max_nodes,
										   node_counter);
		}
	}

	switch (nodeTag(planstate))
	{
		case T_AppendState:
		{
			AppendState *appendstate = (AppendState *) planstate;
			for (i32 i = 0; i < appendstate->as_nplans; i++)
				walk_instrumentation_recursive(appendstate->appendplans[i],
											   metrics, max_nodes,
											   node_counter);
			break;
		}
		case T_MergeAppendState:
		{
			MergeAppendState *mergeappendstate = (MergeAppendState *) planstate;
			for (i32 i = 0; i < mergeappendstate->ms_nplans; i++)
				walk_instrumentation_recursive(mergeappendstate->mergeplans[i],
											   metrics, max_nodes,
											   node_counter);
			break;
		}
		case T_BitmapAndState:
		{
			BitmapAndState *bitmapandstate = (BitmapAndState *) planstate;
			for (i32 i = 0; i < bitmapandstate->nplans; i++)
				walk_instrumentation_recursive(bitmapandstate->bitmapplans[i],
											   metrics, max_nodes,
											   node_counter);
			break;
		}
		case T_BitmapOrState:
		{
			BitmapOrState *bitmaporstate = (BitmapOrState *) planstate;
			for (i32 i = 0; i < bitmaporstate->nplans; i++)
				walk_instrumentation_recursive(bitmaporstate->bitmapplans[i],
											   metrics, max_nodes,
											   node_counter);
			break;
		}
		case T_SubqueryScanState:
		{
			SubqueryScanState *subqueryscan = (SubqueryScanState *) planstate;
			if (subqueryscan->subplan)
			{
				walk_instrumentation_recursive(subqueryscan->subplan, metrics,
											   max_nodes, node_counter);
			}
			break;
		}
		default:
			break;
	}
}
