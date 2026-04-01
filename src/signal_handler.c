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

#include "signal_handler.h"

#include "common.h"
#include "executor/execdesc.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "plan_tree_walker.h"
#include "shared_memory.h"
#include "storage/ipc.h"
#include "storage/spin.h"

/* Static storage for current QueryDesc pointer. */
static volatile QueryDesc *CURRENT_QUERY_DESC = NULL;

/* Previous SIGUSR2 handler for chaining. */
static pqsigfunc PREV_SIGUSR2_HANDLER = NULL;

/* Debug counter for signal handler invocations (async-signal-safe). */
static volatile sig_atomic_t SIGNAL_HANDLER_CALL_COUNT = 0;
static volatile sig_atomic_t SIGNAL_HANDLER_SUCCESS_COUNT = 0;
static volatile sig_atomic_t SIGNAL_HANDLER_NO_QUERYDESC = 0;
static volatile sig_atomic_t SIGNAL_HANDLER_SHMEM_NULL = 0;
static volatile sig_atomic_t SIGNAL_HANDLER_NO_SLOT = 0;

void
pwh_set_current_query_desc(QueryDesc *queryDesc)
{
	CURRENT_QUERY_DESC = queryDesc;
}

QueryDesc *
pwh_get_current_query_desc(void)
{
	return (QueryDesc *) CURRENT_QUERY_DESC;
}

/*
 * SIGUSR2 signal handler.
 *
 * Async-signal-safe refresh of live metrics. Read Instrumentation fields from
 * the PlanState tree and write them directly to a shared memory slot.
 *
 * SAFETY: PlanState tree topology is fixed after ExecutorStart. Only numeric
 * Instrumentation fields change during ExecutorRun. Read those numbers
 * and write to pre-allocated buffer with no syscalls, allocations, or locks.
 */
void
pwh_sigusr2_handler(SIGNAL_ARGS)
{
	int save_errno = errno;
	SIGNAL_HANDLER_CALL_COUNT++;

	/* Check if we have an active query. */
	QueryDesc *queryDesc = (QueryDesc *) CURRENT_QUERY_DESC;

	if (queryDesc == NULL || queryDesc->planstate == NULL)
	{
		SIGNAL_HANDLER_NO_QUERYDESC++;
		goto chain;
	}

	/* Get our slot. It's unsafe to call pwh_get_my_backend_entry() here. */
	if (PWH_SHMEM == NULL)
	{
		SIGNAL_HANDLER_SHMEM_NULL++;
		goto chain;
	}

	PwhSharedMemoryBackendEntry *shmem_be_entry = NULL;
	for (u64 i = 0; i < (u64) PWH_MAX_TRACKED_QUERIES_GUC; i++)
	{
		PwhSharedMemoryBackendEntry *be = PWH_GET_BACKEND_ENTRY_UNSAFE(i);
		if (be->backend_pid == MyProcPid)
		{
			shmem_be_entry = be;
			break;
		}
	}

	if (shmem_be_entry == NULL)
	{
		SIGNAL_HANDLER_NO_SLOT++;
		goto chain;
	}

	/* Refresh instrumentation data. */
	PwhNodeMetrics *metrics = pwh_get_backend_entry_metrics(shmem_be_entry);
	pwh_walk_plan_instrumentation(queryDesc->planstate, metrics,
								  PWH_MAX_NODES_PER_QUERY_GUC);

	/* Increment generation counter to signal completion. */
	PWH_MEMORY_BARRIER();
	shmem_be_entry->poll_generation++;
	SIGNAL_HANDLER_SUCCESS_COUNT++;

chain:
	/* Chain to previous handler if it's a valid function pointer. */
	if (PREV_SIGUSR2_HANDLER && PREV_SIGUSR2_HANDLER != SIG_IGN &&
		PREV_SIGUSR2_HANDLER != SIG_DFL)
	{
		errno = save_errno;
		(*PREV_SIGUSR2_HANDLER)(postgres_signal_arg);
		return;
	}
	errno = save_errno;
}

void
pwh_install_signal_handler(void)
{
	PREV_SIGUSR2_HANDLER = pwh_install_pqsignal(SIGUSR2, pwh_sigusr2_handler);
	if (PREV_SIGUSR2_HANDLER != NULL)
	{
		ereport(DEBUG1,
				(errmsg("PWH: SIGUSR2 handler installed"),
				 errdetail("Previous handler at %p", PREV_SIGUSR2_HANDLER)));
	}
	else
	{
		ereport(DEBUG1, (errmsg("PWH: SIGUSR2 handler installed")));
	}
}

u64
pwh_get_signal_handler_call_count(void)
{
	return (u64) SIGNAL_HANDLER_CALL_COUNT;
}

u64
pwh_get_signal_handler_success_count(void)
{
	return (u64) SIGNAL_HANDLER_SUCCESS_COUNT;
}

u64
pwh_get_signal_handler_no_querydesc(void)
{
	return (u64) SIGNAL_HANDLER_NO_QUERYDESC;
}

u64
pwh_get_signal_handler_shmem_null(void)
{
	return (u64) SIGNAL_HANDLER_SHMEM_NULL;
}

u64
pwh_get_signal_handler_no_slot(void)
{
	return (u64) SIGNAL_HANDLER_NO_SLOT;
}
