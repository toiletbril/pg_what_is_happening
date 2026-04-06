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

#ifndef PWH_PLAN_TREE_WALKER_H
#define PWH_PLAN_TREE_WALKER_H

#include "postgres.h"

#include "common.h"
#include "nodes/execnodes.h"
#include "nodes/nodes.h"
#include "shared_memory.h"

/* Convert NodeTag to human-readable string. */
extern const char *pwh_node_tag_to_string(NodeTag tag);

/* Walk plan tree and populate topology (node_id, parent_id, node_type). */
extern u64 pwh_remember_planstate_tree_as_metric_structure(
	PlanState *planstate, PwhNodeMetrics *metrics, u64 max_nodes,
	i32 parent_id);

/* Walk plan tree and read Instrumentation data. */
extern void pwh_collect_planstate_metrics(PlanState		 *planstate,
										  PwhNodeMetrics *metrics,
										  u64			  max_nodes);

#endif /* PWH_PLAN_TREE_WALKER_H */
