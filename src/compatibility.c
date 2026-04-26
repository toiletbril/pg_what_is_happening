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
 * Helpers to compile the extension on as many PostgreSQL versions as possible.
 * Main code lies in compatibility/ dir.
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

static forceinline const char *
general_tag_to_string(NodeTag tag)
{
	switch (tag)
	{
		case T_Result:
			return "Result";
		case T_ModifyTable:
			return "ModifyTable";
		case T_Append:
			return "Append";
		case T_MergeAppend:
			return "MergeAppend";
		case T_RecursiveUnion:
			return "RecursiveUnion";
		case T_BitmapAnd:
			return "BitmapAnd";
		case T_BitmapOr:
			return "BitmapOr";
		case T_SeqScan:
			return "SeqScan";
		case T_IndexScan:
			return "IndexScan";
		case T_IndexOnlyScan:
			return "IndexOnlyScan";
		case T_BitmapIndexScan:
			return "BitmapIndexScan";
		case T_BitmapHeapScan:
			return "BitmapHeapScan";
		case T_TidScan:
			return "TidScan";
		case T_SubqueryScan:
			return "SubqueryScan";
		case T_FunctionScan:
			return "FunctionScan";
		case T_ValuesScan:
			return "ValuesScan";
		case T_CteScan:
			return "CteScan";
		case T_WorkTableScan:
			return "WorkTableScan";
		case T_ForeignScan:
			return "ForeignScan";
		case T_NestLoop:
			return "NestLoop";
		case T_MergeJoin:
			return "MergeJoin";
		case T_HashJoin:
			return "HashJoin";
		case T_Material:
			return "Material";
		case T_Sort:
			return "Sort";
		case T_Group:
			return "Group";
		case T_Agg:
			return "Agg";
		case T_WindowAgg:
			return "WindowAgg";
		case T_Unique:
			return "Unique";
		case T_Hash:
			return "Hash";
		case T_SetOp:
			return "SetOp";
		case T_LockRows:
			return "LockRows";
		case T_Limit:
			return "Limit";
		default:
			return NULL;
	}
}

const char *
pwh_node_tag_to_string(NodeTag tag)
{
	const char *name = general_tag_to_string(tag);
	if (name != NULL)
		return name;
	name = pwh_node_tag_to_string_inline(tag);
	if (name != NULL)
		return name;
	return "Unknown";
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

	hash ^= (u64) GetCurrentTimestamp() >> 16;
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
