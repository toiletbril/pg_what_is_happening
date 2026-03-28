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

#include "nodes/execnodes.h"
#include "nodes/nodes.h"

const char *
pwh_node_tag_to_string(NodeTag tag)
{
	return pwh_node_tag_to_string_inline(tag);
}

bool
pwh_walk_planstate_children(PlanState *planstate, PwhNodeVisitorFn visitor,
							void *context)
{
	return pwh_walk_planstate_children_inline(planstate, visitor, context);
}

u64
pwh_compute_query_id(const char *query_text)
{
	if (query_text == NULL)
	{
		return 0;
	}
	return pwh_hash_djb2(5381, (const u8 *) query_text, strlen(query_text));
}

pqsigfunc
pwh_install_pqsignal(int signo, pqsigfunc func)
{
#if PG_VERSION_NUM >= 190000
	pqsignal(signo, func);
	return NULL;
#else
	return pqsignal(signo, func);
#endif
}
