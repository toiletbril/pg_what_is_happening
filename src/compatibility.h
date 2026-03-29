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
#include "nodes/nodes.h"
#include "storage/lwlock.h"

/* Greenplum detection. */
#ifdef GP_VERSION_NUM
#define PWH_IS_GREENPLUM 1
#else
#define PWH_IS_GREENPLUM 0
#endif

/* Forward declarations for compatibility headers. */
typedef struct PlanState PlanState;
typedef bool (*PwhNodeVisitorFn)(PlanState *planstate, void *context);

extern bool pwh_walk_planstate_recursive(PlanState		 *planstate,
										 PwhNodeVisitorFn visitor,
										 void			 *context);

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
#define PWH_INSTR_TIME_MAYBE_GET_DOUBLE(n) (INSTR_TIME_GET_DOUBLE(n))
#else
#define PWH_INSTR_TIME_MAYBE_GET_DOUBLE(n) (n)
#endif

/* Query ID computation fallback, used when queryId is 0. */
extern u64 pwh_compute_query_id(const char *query_text);

extern const char *pwh_node_tag_to_string(NodeTag tag);

extern bool pwh_walk_planstate_children(PlanState		*planstate,
										PwhNodeVisitorFn visitor,
										void			*context);

extern pqsigfunc pwh_install_pqsignal(int signo, pqsigfunc func);

#endif /* PWH_COMPATIBILITY_H. */
