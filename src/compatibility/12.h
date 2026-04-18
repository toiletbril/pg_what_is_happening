
#ifndef PWH_COMPAT_12_H
#define PWH_COMPAT_12_H

#include "nodes/execnodes.h"

#define PWH_GET_QUERY_ID(plannedstmt) ((plannedstmt)->queryId)

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
#define PWH_SHMEM_REQUEST_IN_STARTUP_HOOK()             \
	do                                                  \
	{                                                   \
		PWH_REQUEST_LWLOCKS("pg_what_is_happening", 1); \
		RequestAddinShmemSpace(PWH_SHMEM_SIZE);         \
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

static forceinline const char *
pwh_node_tag_to_string_inline(NodeTag tag)
{
	switch (tag)
	{
		case T_CustomScan:
			return "CustomScan";
		case T_Gather:
			return "Gather";
		case T_GatherMerge:
			return "GatherMerge";
		case T_ProjectSet:
			return "ProjectSet";
		case T_TableFuncScan:
			return "TableFuncScan";
		case T_NamedTuplestoreScan:
			return "NamedTuplestoreScan";
		default:
			return NULL;
	}
}

#endif /* PWH_COMPAT_12_H */
