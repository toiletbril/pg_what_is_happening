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

#include "openmetrics.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "compatibility.h"
#include "http_server.h"
#include "postmaster/bgworker.h"
#include "shared_memory.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"

static void	 pwh_metrics_handler(const HttpRequest *req, HttpResponse *resp,
								 void *user_data);
static char *pwh_format_openmetrics(void);

static void
pwh_bgworker_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;
	proc_exit(0);
	errno = save_errno;
}

void
pwh_register_openmetrics_worker(void)
{
	BackgroundWorker worker;

	memset(&worker, 0, sizeof(BackgroundWorker));
	snprintf(worker.bgw_name, BGW_MAXLEN,
			 "pg_what_is_happening openmetrics exporter");
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | PWH_BGWORKER_BYPASS_ALLOWCONN;
	worker.bgw_start_time = BgWorkerStart_PostmasterStart;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_what_is_happening");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "pwh_openmetrics_main");
	worker.bgw_main_arg = (Datum) 0;
	worker.bgw_notify_pid = 0;

	RegisterBackgroundWorker(&worker);
}

void
pwh_openmetrics_main(Datum main_arg)
{
	pqsignal(SIGTERM, pwh_bgworker_sigterm);
	BackgroundWorkerUnblockSignals();

	bool was_found;
	PWH_SHMEM = ShmemInitStruct("pg_what_is_happening",
								pwh_shared_memory_size(), &was_found);
	if (!PWH_SHMEM || !was_found)
	{
		elog(ERROR, "PWH: Background worker failed to attach to shared memory");
		proc_exit(1);
	}
	elog(LOG, "PWH: Background worker attached to shared memory");

	const char *listen_addr =
		PWH_GET_GUC("pg_what_is_happening.listen_address");

	if (!listen_addr || listen_addr[0] == '\0')
		listen_addr = "127.0.0.1:9187";

	elog(LOG, "PWH: Starting openmetrics exporter on %s", listen_addr);

	HttpServer *server = pwh_http_server_create(listen_addr);
	if (server == NULL)
	{
		elog(ERROR, "PWH: Failed to create HTTP server on %s", listen_addr);
		proc_exit(1);
	}

	elog(LOG, "PWH: Metrics endpoint listening on %s", listen_addr);

	pwh_http_server_set_handler(server, pwh_metrics_handler, NULL);
	pwh_http_server_run(server);
	pwh_http_server_destroy(server);

	elog(LOG, "PWH: Metrics endpoint shutting down");
	proc_exit(0);
}

static void
pwh_metrics_handler(const HttpRequest *req, HttpResponse *resp, void *user_data)
{
	char *metrics;

	unused(user_data);

	if (strcmp(req->method, "GET") != 0 || strcmp(req->path, "/metrics") != 0)
	{
		pwh_http_response_text(resp, 404, "Not Found");
		return;
	}

	/* Check shared memory is available. */
	if (PWH_SHMEM == NULL)
	{
		elog(WARNING, "PWH: Shared memory not initialized in metrics handler");
		pwh_http_response_text(resp, 503, "Service Unavailable");
		return;
	}

	/* Trigger async refresh via signal handler before reading data. */
	i32 signaled = 0;
	for (i32 i = 0; i < pwh_get_backend_entry_count(); i++)
	{
		PwhSharedMemoryBackendEntry *shmem_be_entry = pwh_get_backend_entry(i);

		if (shmem_be_entry && shmem_be_entry->is_query_active &&
			shmem_be_entry->backend_pid != 0)
		{
			elog(LOG, "PWH: Sending SIGUSR2 to PID=%d query_id=%lu gen=%lu",
				 shmem_be_entry->backend_pid,
				 (unsigned long) shmem_be_entry->query_id,
				 (unsigned long) shmem_be_entry->poll_generation);
			kill(shmem_be_entry->backend_pid, SIGUSR2);
			signaled++;
		}
	}

	elog(LOG, "PWH: Sent SIGUSR2 to %d active backends", signaled);

	/* Generation counter will increment after handler runs.. */
	usleep(10000);

	metrics = pwh_format_openmetrics();
	if (metrics == NULL)
	{
		elog(WARNING, "PWH: Failed to format metrics");
		pwh_http_response_text(resp, 500, "Internal Server Error");
		return;
	}

	pwh_http_response_text(resp, 200, metrics);
	resp->free_body = true;
}

static char *
pwh_format_openmetrics(void)
{
	if (PWH_SHMEM == NULL)
	{
		elog(WARNING, "PWH: PWH_SHMEM is NULL in format_openmetrics");
		return NULL;
	}

	usize buffer_size = 65536;
	char *buffer = (char *) palloc(buffer_size);

	usize offset = 0;

	offset += snprintf(
		buffer + offset, buffer_size - offset,
		"# HELP pg_what_is_happening_active_query_node_rows Rows produced "
		"by active query plan node\n"
		"# TYPE pg_what_is_happening_active_query_node_rows gauge\n"
		"# HELP pg_what_is_happening_active_query_node_time_seconds "
		"Execution time for active query plan node\n"
		"# TYPE pg_what_is_happening_active_query_node_time_seconds gauge\n"
		"# HELP pg_what_is_happening_active_query_node_time_percent "
		"Percentage of total query time spent in this node\n"
		"# TYPE pg_what_is_happening_active_query_node_time_percent gauge\n"
		"# HELP pg_what_is_happening_active_query_node_shared_hit Shared "
		"buffer hits\n"
		"# TYPE pg_what_is_happening_active_query_node_shared_hit gauge\n"
		"# HELP pg_what_is_happening_active_query_node_shared_read Shared "
		"buffer reads\n"
		"# TYPE pg_what_is_happening_active_query_node_shared_read gauge\n"
		"# HELP pg_what_is_happening_active_query_node_temp_read Temp blocks "
		"read (spills)\n"
		"# TYPE pg_what_is_happening_active_query_node_temp_read gauge\n"
		"# HELP pg_what_is_happening_active_query_node_temp_written Temp "
		"blocks written (spills)\n"
		"# TYPE pg_what_is_happening_active_query_node_temp_written gauge\n");

	for (i32 i = 0; i < pwh_get_backend_entry_count(); i++)
	{
		PwhSharedMemoryBackendEntry *shmem_be_entry = pwh_get_backend_entry(i);

		if (!shmem_be_entry || shmem_be_entry->backend_pid == 0)
			continue;

		elog(
			LOG,
			"PWH: Formatting backend entry %d PID=%d active=%d query_id=%lu num_nodes=%d",
			i, shmem_be_entry->backend_pid, shmem_be_entry->is_query_active,
			(unsigned long) shmem_be_entry->query_id,
			shmem_be_entry->num_nodes);

		if (!shmem_be_entry->is_query_active)
			continue;

		double total_query_time = 0.0;
		for (i32 j = 0; j < shmem_be_entry->num_nodes; j++)
			total_query_time +=
				shmem_be_entry->plan_nodes[j].execution.total_time_us;

		for (i32 j = 0; j < shmem_be_entry->num_nodes; j++)
		{
			PwhNode *node = &shmem_be_entry->plan_nodes[j];

			if (unlikely(offset + 1024 >= buffer_size))
			{
				buffer_size *= 2;
				buffer = (char *) repalloc(buffer, buffer_size);
			}

			double node_time_seconds =
				node->execution.total_time_us / 1000000.0;
			double node_percent =
				total_query_time > 0.0
					? (node->execution.total_time_us / total_query_time) * 100.0
					: 0.0;

			offset += snprintf(
				buffer + offset, buffer_size - offset,
				"pg_what_is_happening_active_query_node_rows{pid=\"%d\","
				"query_id=\"%lu\",node_id=\"%d\",node_tag=\"%s\"} %.0f\n",
				shmem_be_entry->backend_pid,
				(unsigned long) shmem_be_entry->query_id, node->node_id,
				node->node_type_name, node->execution.tuples_returned);

			offset += snprintf(
				buffer + offset, buffer_size - offset,
				"pg_what_is_happening_active_query_node_time_seconds{pid=\"%d\","
				"query_id=\"%lu\",node_id=\"%d\",node_tag=\"%s\"} %.6f\n",
				shmem_be_entry->backend_pid,
				(unsigned long) shmem_be_entry->query_id, node->node_id,
				node->node_type_name, node_time_seconds);

			offset += snprintf(
				buffer + offset, buffer_size - offset,
				"pg_what_is_happening_active_query_node_time_percent{pid=\"%d\","
				"query_id=\"%lu\",node_id=\"%d\",node_tag=\"%s\"} %.2f\n",
				shmem_be_entry->backend_pid,
				(unsigned long) shmem_be_entry->query_id, node->node_id,
				node->node_type_name, node_percent);

			offset += snprintf(
				buffer + offset, buffer_size - offset,
				"pg_what_is_happening_active_query_node_shared_hit{pid=\"%d\","
				"query_id=\"%lu\",node_id=\"%d\",node_tag=\"%s\"} %ld\n",
				shmem_be_entry->backend_pid,
				(unsigned long) shmem_be_entry->query_id, node->node_id,
				node->node_type_name, (long) node->buffer_usage.shared_hit);

			offset += snprintf(
				buffer + offset, buffer_size - offset,
				"pg_what_is_happening_active_query_node_shared_read{pid=\"%d\","
				"query_id=\"%lu\",node_id=\"%d\",node_tag=\"%s\"} %ld\n",
				shmem_be_entry->backend_pid,
				(unsigned long) shmem_be_entry->query_id, node->node_id,
				node->node_type_name, (long) node->buffer_usage.shared_read);

			offset += snprintf(
				buffer + offset, buffer_size - offset,
				"pg_what_is_happening_active_query_node_temp_read{pid=\"%d\","
				"query_id=\"%lu\",node_id=\"%d\",node_tag=\"%s\"} %ld\n",
				shmem_be_entry->backend_pid,
				(unsigned long) shmem_be_entry->query_id, node->node_id,
				node->node_type_name, (long) node->buffer_usage.temp_read);

			offset += snprintf(
				buffer + offset, buffer_size - offset,
				"pg_what_is_happening_active_query_node_temp_written{pid=\"%d\","
				"query_id=\"%lu\",node_id=\"%d\",node_tag=\"%s\"} %ld\n",
				shmem_be_entry->backend_pid,
				(unsigned long) shmem_be_entry->query_id, node->node_id,
				node->node_type_name, (long) node->buffer_usage.temp_written);
		}
	}

	return buffer;
}
