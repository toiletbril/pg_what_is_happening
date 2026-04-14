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

#include "compatibility.h"

#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/nodes.h"
#include "utils/timestamp.h"

/*
 * compatibility.h provides one inline shim:
 *
 * static inline const char *pwh_node_tag_to_string_inline(NodeTag tag);
 */

const char *
pwh_node_tag_to_string(NodeTag tag)
{
	const char *name = node_to_name(tag);
	if (name != NULL)
		return name;
	name = pwh_node_tag_to_string_inline(tag);
	if (name != NULL)
		return name;
	return "Unknown";
}

bool
pwh_walk_planstate_children(PlanState *planstate, PwhNodeVisitorFn visitor,
							void *context)
{
	if (IsA(planstate->plan, CteScan))
	{
		CteScanState *cs = (CteScanState *) planstate;
		if (cs->cteplanstate != NULL)
		{
			if (!pwh_walk_planstate_recursive(cs->cteplanstate, visitor,
											  context))
				return false;
		}
	}

	switch (nodeTag(planstate))
	{
		case T_AppendState:
		{
			AppendState *as = (AppendState *) planstate;
			for (i32 i = 0; i < as->as_nplans; i++)
			{
				if (!pwh_walk_planstate_recursive(as->appendplans[i], visitor,
												  context))
					return false;
			}
			break;
		}
		case T_MergeAppendState:
		{
			MergeAppendState *mas = (MergeAppendState *) planstate;
			for (i32 i = 0; i < mas->ms_nplans; i++)
			{
				if (!pwh_walk_planstate_recursive(mas->mergeplans[i], visitor,
												  context))
					return false;
			}
			break;
		}
		case T_BitmapAndState:
		{
			BitmapAndState *bas = (BitmapAndState *) planstate;
			for (i32 i = 0; i < bas->nplans; i++)
			{
				if (!pwh_walk_planstate_recursive(bas->bitmapplans[i], visitor,
												  context))
					return false;
			}
			break;
		}
		case T_BitmapOrState:
		{
			BitmapOrState *bos = (BitmapOrState *) planstate;
			for (i32 i = 0; i < bos->nplans; i++)
			{
				if (!pwh_walk_planstate_recursive(bos->bitmapplans[i], visitor,
												  context))
					return false;
			}
			break;
		}
		case T_SubqueryScanState:
		{
			SubqueryScanState *sqs = (SubqueryScanState *) planstate;
			if (sqs->subplan != NULL)
			{
				if (!pwh_walk_planstate_recursive(sqs->subplan, visitor,
												  context))
					return false;
			}
			break;
		}
		default:
			break;
	}

	return true;
}

u64
pwh_compute_query_id(const QueryDesc *qd)
{
	u64 hash = 5381;
	if (qd->sourceText != NULL && *qd->sourceText != '\0')
	{
		hash = pwh_hash_djb2(hash, (const u8 *) qd->sourceText,
							 strlen(qd->sourceText));
	}

	hash ^= (u64) GetCurrentTimestamp() >> 32;
	hash ^= MyProcPid;

	return hash;
}

pqsigfunc
pwh_install_pqsignal(int signo, pqsigfunc func)
{
#if PG_VERSION_NUM >= 180000 || PG_VERSION_NUM < 100000
	pqsignal(signo, func);
	return NULL;
#else
	return pqsignal(signo, func);
#endif
}
