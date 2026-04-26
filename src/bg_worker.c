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

#include "bg_worker.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "compatibility.h"
#include "gucs.h"
#include "http_server.h"
#include "metrics.h"
#include "postmaster/bgworker.h"
#include "shared_memory.h"
#include "storage/ipc.h"

static void metrics_handler(const HttpRequest *req, HttpResponse *resp,
							void *user_data);

static void handle_sigterm(SIGNAL_ARGS);

static HttpServer *HTTP_SERVER_INSTANCE = NULL;

#define BG_WORKER_PROCESS_NAME "pg_what_is_happening openmetrics exporter"
#define BG_WORKER_ENTRY_FUNCTION "pwh_bgworker_main"

void
pwh_register_openmetrics_exporter_as_bg_worker(void)
{
	BackgroundWorker w;

	memset(&w, 0, sizeof(BackgroundWorker));
	snprintf(w.bgw_name, BGW_MAXLEN, BG_WORKER_PROCESS_NAME);
	w.bgw_flags = BGWORKER_SHMEM_ACCESS | PWH_BGWORKER_BYPASS_ALLOWCONN;
	w.bgw_start_time = BgWorkerStart_PostmasterStart;
	w.bgw_restart_time = 2;
	snprintf(w.bgw_library_name, BGW_MAXLEN, "pg_what_is_happening");
	snprintf(w.bgw_function_name, BGW_MAXLEN, BG_WORKER_ENTRY_FUNCTION);
	w.bgw_main_arg = (Datum) 0;
	w.bgw_notify_pid = 0;

	RegisterBackgroundWorker(&w);
}

wontreturn void
pwh_bgworker_main(Datum main_arg)
{
	unused(main_arg);

	pqsignal(SIGTERM, handle_sigterm);
	BackgroundWorkerUnblockSignals();

	PWH_SHMEM = pwh_get_shared_memory_ptr();
	ereport(LOG, (errmsg("PWH: Background worker attached to shared memory")));

	const char *listen_addr = PWH_GUC_METRICS_LISTEN_ADDRESS;

	Assert(listen_addr != NULL);

	if (listen_addr[0] == '\0')
	{
		listen_addr = "127.0.0.1:9187";
	}

	ereport(LOG,
			(errmsg("PWH: Starting openmetrics exporter on %s", listen_addr)));

	HTTP_SERVER_INSTANCE = pwh_http_server_create(listen_addr);

	if (HTTP_SERVER_INSTANCE == NULL)
	{
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PWH: No HTTP backend compiled in"),
				 errdetail("Recompile with HTTP_BACKEND=mongoose or similar")));
	}

	pwh_http_server_set_handler(HTTP_SERVER_INSTANCE, metrics_handler, NULL);
	pwh_http_server_run(HTTP_SERVER_INSTANCE); /* Blocking. */
	pwh_http_server_destroy(HTTP_SERVER_INSTANCE);

	ereport(LOG, (errmsg("PWH: Metrics endpoint shutting down")));

	proc_exit(0);
}

/* XXX make HTTP replies more cool. */
static void
metrics_handler(const HttpRequest *req, HttpResponse *resp, void *user_data)
{
	unused(user_data);

	if (!streq(req->method, "GET") || !streq(req->path, "/metrics"))
	{
		pwh_http_response_set_text(resp, 404, "Unknown Path");
		return;
	}

	/* Lock the shared memory until we are done reading. */
	PWH_LWLOCK_ACQUIRE(PWH_SHMEM->entry_search_lock, LW_SHARED);

	u32 n_cleaned = pwh_cleanup_orphaned_slots();
	if (n_cleaned > 0)
	{
		ereport(DEBUG1,
				(errmsg("PWH: Cleaned up %u orphaned slots", n_cleaned)));
	}

	u32 n_signaled = pwh_request_backend_metrics_unlocked();

	ereport(DEBUG2,
			(errmsg("PWH: Sent SIGUSR2 to %u active backends", n_signaled)));

	usleep((useconds_t) (PWH_GUC_SIGNAL_TIMEOUT_MS * 1000));

	char *metrics = pwh_format_openmetrics();

	PWH_LWLOCK_RELEASE(PWH_SHMEM->entry_search_lock);

	if (metrics == NULL)
	{
		ereport(WARNING, (errmsg("PWH: Failed to format metrics")));
		pwh_http_response_set_text(resp, 500, "Internal Server Error");
		return;
	}

	pwh_http_response_set_text(resp, 200, metrics);

	pfree(metrics);
}

wontreturn static void
handle_sigterm(SIGNAL_ARGS)
{
	if (HTTP_SERVER_INSTANCE != NULL)
	{
		pwh_http_server_stop(HTTP_SERVER_INSTANCE);
	}

	proc_exit(0);
}
