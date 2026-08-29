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
 * Lazy-async approach of collection backend metrics. All backends have a
 * SIGUSR2 signal installed, which forces the backends to write the metrics to
 * the shared memory. We send a SIGUSR2 signal to all backends, all of which
 * provides us the ability to interrupt the queries whenever we want in an
 * totally asynchronous way.
 */

#include "postgres.h"

#include "signal_handler.h"

#include "common.h"
#include "executor/execdesc.h"
#include "gucs.h"
#include "miscadmin.h"
#include "plan_tree_walker.h"
#include "postmaster/bgworker.h"
#include "shared_memory.h"

/* Static storage for current QueryDesc pointer. */
static volatile QueryDesc					*CURRENT_QUERY_DESC = NULL;
static volatile PwhSharedMemoryBackendEntry *CURRENT_ENTRY = NULL;
static PwhNodeInstrumentation			   **CURRENT_INSTRUMENTATION = NULL;
static volatile sig_atomic_t				 CURRENT_INSTRUMENTATION_COUNT = 0;

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

void
pwh_set_signal_metrics(PwhSharedMemoryBackendEntry *entry,
					   PwhNodeInstrumentation **instrumentation, u32 count)
{
	CURRENT_INSTRUMENTATION = instrumentation;
	CURRENT_INSTRUMENTATION_COUNT = (sig_atomic_t) count;
	PWH_MEMORY_BARRIER();
	CURRENT_ENTRY = entry;
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
	if (SIGNAL_HANDLER_CALL_COUNT < SIG_ATOMIC_MAX)
		SIGNAL_HANDLER_CALL_COUNT++;

	PwhSharedMemoryBackendEntry *entry =
		(PwhSharedMemoryBackendEntry *) CURRENT_ENTRY;
	PwhNodeInstrumentation **instrumentation = CURRENT_INSTRUMENTATION;
	sig_atomic_t			 count = CURRENT_INSTRUMENTATION_COUNT;

	if (entry == NULL || instrumentation == NULL || count <= 0)
	{
		if (SIGNAL_HANDLER_NO_QUERYDESC < SIG_ATOMIC_MAX)
			SIGNAL_HANDLER_NO_QUERYDESC++;
		goto chain;
	}

	if (PWH_SHMEM == NULL)
	{
		if (SIGNAL_HANDLER_SHMEM_NULL < SIG_ATOMIC_MAX)
			SIGNAL_HANDLER_SHMEM_NULL++;
		goto chain;
	}

	if (entry->backend_pid != MyProcPid)
	{
		if (SIGNAL_HANDLER_NO_SLOT < SIG_ATOMIC_MAX)
			SIGNAL_HANDLER_NO_SLOT++;
		goto chain;
	}

	/* Refresh instrumentation data. */
	PwhNodeMetrics *metrics = pwh_get_backend_entry_metrics(entry);
	sig_atomic_t	base_sequence;
	if (!pwh_begin_metrics_write(entry, &base_sequence))
		goto chain;
	pwh_collect_instrumentation_metrics(instrumentation, metrics, (u64) count);
	pwh_end_metrics_write(entry, base_sequence);

	/* Increment generation counter to signal completion. */
	pwh_advance_poll_generation(entry);

	if (SIGNAL_HANDLER_SUCCESS_COUNT < SIG_ATOMIC_MAX)
		SIGNAL_HANDLER_SUCCESS_COUNT++;

chain:
	errno = save_errno;

	/* Chain to previous handler if it's a valid function pointer. */
	if (PREV_SIGUSR2_HANDLER && PREV_SIGUSR2_HANDLER != PWH_SIG_IGN &&
		PREV_SIGUSR2_HANDLER != PWH_SIG_DFL)
	{
		PWH_CALL_SIGNAL_HANDLER(PREV_SIGUSR2_HANDLER);
	}
}

static volatile bool WAS_SIGNAL_HANDLER_INSTALLED = false;

void
pwh_install_signal_handler(void)
{
	/* Signal handler should only be installed in regular backends. */
	Assert(IsUnderPostmaster);

	if (!WAS_SIGNAL_HANDLER_INSTALLED)
	{
		PREV_SIGUSR2_HANDLER =
			pwh_install_pqsignal(SIGUSR2, pwh_sigusr2_handler);
		ereport(DEBUG1, (errmsg("PWH: SIGUSR2 handler installed")));
		WAS_SIGNAL_HANDLER_INSTALLED = true;
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
