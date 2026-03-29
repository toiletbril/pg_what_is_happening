
#ifndef PWH_COMPAT_13_H
#define PWH_COMPAT_13_H

#include "nodes/execnodes.h"

#define PWH_GET_QUERY_ID(plannedstmt) ((plannedstmt)->queryId)

typedef struct
{
	LWLock lock;
} PwhSharedMemoryHeader;

#define PWH_LWLOCK_ACQUIRE(lock, mode) LWLockAcquire(&(lock), mode)
#define PWH_LWLOCK_RELEASE(lock) LWLockRelease(&(lock))
#define PWH_LWLOCK_INITIALIZE(lock, tranche_id) \
	LWLockInitialize(&(lock), tranche_id)
#define PWH_REQUEST_LWLOCKS(name, count) RequestNamedLWLockTranche(name, count)
#define PWH_LWLOCK_TRANCHE_ID_DECL static i32 PWH_LWLOCK_TRANCHE_ID = 0
#define PWH_LWLOCK_SETUP_TRANCHE(var, name)   \
	do                                        \
	{                                         \
		(var) = LWLockNewTrancheId();         \
		LWLockRegisterTranche((var), (name)); \
	} while (0)

#define PWH_GET_GUC(name) GetConfigOptionByName(name, NULL, false)

#define PWH_SHMEM_REQUEST_HOOK_DECL		 /* no shmem_request_hook in PG < 15 */
#define PWH_INSTALL_SHMEM_REQUEST_HOOK() /* no shmem_request_hook in PG < 15 \
										  */
#define PWH_SHMEM_REQUEST_IN_STARTUP_HOOK()                   \
	do                                                        \
	{                                                         \
		PWH_REQUEST_LWLOCKS("pg_what_is_happening", 1);       \
		RequestAddinShmemSpace(pwh_get_shared_memory_size()); \
	} while (0)

#define PWH_CREATE_TUPLE_DESC(natts) CreateTemplateTupleDesc(natts)
#define PWH_TUPLE_DESC_FINALIZE(tupdesc) /* no TupleDescFinalize in PG < 19 */

#define PWH_BGWORKER_BYPASS_ALLOWCONN 0

#define PWH_COPY_BUFUSAGE(metrics, instr, idx)           \
	do                                                   \
	{                                                    \
		(metrics)[idx].buffer_usage.cache_hits =         \
			(instr)->bufusage.shared_blks_hit;           \
		(metrics)[idx].buffer_usage.cache_misses =       \
			(instr)->bufusage.shared_blks_read;          \
		(metrics)[idx].buffer_usage.local_cache_hits =   \
			(instr)->bufusage.local_blks_hit;            \
		(metrics)[idx].buffer_usage.local_cache_misses = \
			(instr)->bufusage.local_blks_read;           \
		(metrics)[idx].buffer_usage.spill_file_reads =   \
			(instr)->bufusage.temp_blks_read;            \
		(metrics)[idx].buffer_usage.spill_file_writes =  \
			(instr)->bufusage.temp_blks_written;         \
	} while (0)

static forceinline bool
pwh_walk_planstate_children_inline(PlanState	   *planstate,
								   PwhNodeVisitorFn visitor, void *context)
{
	switch (nodeTag(planstate))
	{
		case T_AppendState:
		{
			AppendState *appendstate = (AppendState *) planstate;
			for (i32 i = 0; i < appendstate->as_nplans; i++)
			{
				if (!visitor(appendstate->appendplans[i], context))
				{
					return false;
				}
			}
			return true;
		}
		case T_MergeAppendState:
		{
			MergeAppendState *mergeappendstate = (MergeAppendState *) planstate;
			for (i32 i = 0; i < mergeappendstate->ms_nplans; i++)
			{
				if (!visitor(mergeappendstate->mergeplans[i], context))
				{
					return false;
				}
			}
			return true;
		}
		case T_BitmapAndState:
		{
			BitmapAndState *bitmapandstate = (BitmapAndState *) planstate;
			for (i32 i = 0; i < bitmapandstate->nplans; i++)
			{
				if (!visitor(bitmapandstate->bitmapplans[i], context))
				{
					return false;
				}
			}
			return true;
		}
		case T_BitmapOrState:
		{
			BitmapOrState *bitmaporstate = (BitmapOrState *) planstate;
			for (i32 i = 0; i < bitmaporstate->nplans; i++)
			{
				if (!visitor(bitmaporstate->bitmapplans[i], context))
				{
					return false;
				}
			}
			return true;
		}
		case T_SubqueryScanState:
		{
			SubqueryScanState *subqueryscan = (SubqueryScanState *) planstate;
			if (subqueryscan->subplan != NULL)
			{
				if (!visitor(subqueryscan->subplan, context))
				{
					return false;
				}
			}
			return true;
		}
		default:
			return true;
	}
}


static forceinline const char *
pwh_node_tag_to_string_inline(NodeTag tag)
{
	switch (tag)
	{
		case T_Result:
			return "Result";
		case T_ProjectSet:
			return "ProjectSet";
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
		case T_TableFuncScan:
			return "TableFuncScan";
		case T_CteScan:
			return "CteScan";
		case T_NamedTuplestoreScan:
			return "NamedTuplestoreScan";
		case T_WorkTableScan:
			return "WorkTableScan";
		case T_ForeignScan:
			return "ForeignScan";
		case T_CustomScan:
			return "CustomScan";
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
		case T_IncrementalSort:
			return "IncrementalSort";
		case T_Group:
			return "Group";
		case T_Agg:
			return "Agg";
		case T_WindowAgg:
			return "WindowAgg";
		case T_Unique:
			return "Unique";
		case T_Gather:
			return "Gather";
		case T_GatherMerge:
			return "GatherMerge";
		case T_Hash:
			return "Hash";
		case T_SetOp:
			return "SetOp";
		case T_LockRows:
			return "LockRows";
		case T_Limit:
			return "Limit";
		default:
			return "Unknown";
	}
}

#endif /* PWH_COMPAT_13_H. */
