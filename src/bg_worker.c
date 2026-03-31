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
#include <stdarg.h>
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

typedef enum
{
	METRIC_START,
	METRIC_ROWS = METRIC_START,
	METRIC_TIME_SECONDS,
	METRIC_TIME_PERCENT,
	METRIC_CACHE_HITS,
	METRIC_CACHE_MISSES,
	METRIC_LOCAL_CACHE_HITS,
	METRIC_LOCAL_CACHE_MISSES,
	METRIC_SPILL_FILE_READS,
	METRIC_SPILL_FILE_WRITES,
	METRIC_COUNT
} MetricType;

typedef struct
{
	char *data;
	u64	  offset;
	u64	  size;
} FormatterBuffer;

typedef struct
{
	FormatterBuffer *buffer;
	i32				 pid;
	u64				 query_id;
} Formatter;

static void		   metrics_handler(const HttpRequest *req, HttpResponse *resp,
								   void *user_data);
static char		  *format_openmetrics(void);
static void		   buffer_init(FormatterBuffer *buf);
static void		   formatter_init(Formatter *fmt, FormatterBuffer *buf, i32 pid,
								  u64 query_id);
static char		  *discard_buffer(FormatterBuffer *buf);
static const char *metric_suffix(MetricType type);
static const char *metric_help(MetricType type);
static void		   append_metric(Formatter *fmt, PwhNode *node, MetricType type,
								 const char *value_fmt, ...);
static void		   buffer_append(FormatterBuffer *buf, const char *fmt, ...);
static void		   buffer_ensure_capacity(FormatterBuffer *buf, u64 needed);

static void		   handle_sigterm(SIGNAL_ARGS);

static HttpServer *PWH_SERVER = NULL;

void
pwh_register_openmetrics_worker(void)
{
	BackgroundWorker w;

	memset(&w, 0, sizeof(BackgroundWorker));
	snprintf(w.bgw_name, BGW_MAXLEN,
			 "pg_what_is_happening openmetrics exporter");
	w.bgw_flags = BGWORKER_SHMEM_ACCESS | PWH_BGWORKER_BYPASS_ALLOWCONN;
	w.bgw_start_time = BgWorkerStart_PostmasterStart;
	w.bgw_restart_time = 2;
	snprintf(w.bgw_library_name, BGW_MAXLEN, "pg_what_is_happening");
	snprintf(w.bgw_function_name, BGW_MAXLEN, "pwh_bgworker_main");
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
	elog(LOG, "PWH: Background worker attached to shared memory");

	const char *listen_addr =
		PWH_GET_GUC("pg_what_is_happening.listen_address");

	Assert(listen_addr != NULL);

	if (listen_addr[0] == '\0')
	{
		listen_addr = "127.0.0.1:9187";
	}

	elog(LOG, "PWH: Starting openmetrics exporter on %s", listen_addr);

	PWH_SERVER = pwh_http_server_create(listen_addr);

	if (PWH_SERVER == NULL)
	{
		elog(ERROR, "PWH: Failed to create HTTP server on %s", listen_addr);
		proc_exit(1);
	}

	elog(LOG, "PWH: Metrics endpoint listening on %s", listen_addr);

	pwh_http_server_set_handler(PWH_SERVER, metrics_handler, NULL);
	pwh_http_server_run(PWH_SERVER); /* Blocking. */
	pwh_http_server_destroy(PWH_SERVER);

	elog(LOG, "PWH: Metrics endpoint shutting down");

	proc_exit(0);
}

static void
metrics_handler(const HttpRequest *req, HttpResponse *resp, void *user_data)
{
	unused(user_data);

	if (strcmp(req->method, "GET") != 0 || strcmp(req->path, "/metrics") != 0)
	{
		pwh_http_response_text(resp, 404, "Not Found");
		return;
	}

	u32 signaled = pwh_request_backend_metrics();

	elog(LOG, "PWH: Sent SIGUSR2 to %u active backends", signaled);

	usleep((useconds_t) (PWH_SIGNAL_TIMEOUT_MS_GUC * 1000));

	char *metrics = format_openmetrics();

	if (metrics == NULL)
	{
		elog(WARNING, "PWH: Failed to format metrics");
		pwh_http_response_text(resp, 500, "Internal Server Error");
		return;
	}

	pwh_http_response_text(resp, 200, metrics);

	pfree(metrics);
}

#define METRIC_BUFFER_SIZE 65536

static const char *
metric_suffix(MetricType type)
{
	switch (type)
	{
		case METRIC_ROWS:
			return "rows";
		case METRIC_TIME_SECONDS:
			return "time_seconds";
		case METRIC_TIME_PERCENT:
			return "time_percent";
		case METRIC_CACHE_HITS:
			return "cache_hits";
		case METRIC_CACHE_MISSES:
			return "cache_misses";
		case METRIC_LOCAL_CACHE_HITS:
			return "local_cache_hits";
		case METRIC_LOCAL_CACHE_MISSES:
			return "local_cache_misses";
		case METRIC_SPILL_FILE_READS:
			return "spill_file_reads";
		case METRIC_SPILL_FILE_WRITES:
			return "spill_file_writes";
		case METRIC_COUNT:
			break;
	}
	return "unknown";
}

static const char *
metric_help(MetricType type)
{
	switch (type)
	{
		case METRIC_ROWS:
			return "Rows produced by active query plan node";
		case METRIC_TIME_SECONDS:
			return "Execution time for active query plan node";
		case METRIC_TIME_PERCENT:
			return "Percentage of total query time spent in this node";
		case METRIC_CACHE_HITS:
			return "Shared buffer cache hits";
		case METRIC_CACHE_MISSES:
			return "Shared buffer cache misses";
		case METRIC_LOCAL_CACHE_HITS:
			return "Local buffer cache hits";
		case METRIC_LOCAL_CACHE_MISSES:
			return "Local buffer cache misses";
		case METRIC_SPILL_FILE_READS:
			return "Blocks read from spill files";
		case METRIC_SPILL_FILE_WRITES:
			return "Blocks written to spill files";
		case METRIC_COUNT:
			break;
	}
	return "Unknown metric";
}

static void
buffer_init(FormatterBuffer *buf)
{
	buf->offset = 0;
	buf->size = METRIC_BUFFER_SIZE;
	buf->data = (char *) palloc(buf->size);

	for (u32 i = METRIC_START; i < METRIC_COUNT; i++)
	{
		const char *suffix = metric_suffix((MetricType) i);
		const char *help = metric_help((MetricType) i);

		buffer_append(
			buf,
			"# HELP pg_what_is_happening_active_query_node_%s %s\n"
			"# TYPE pg_what_is_happening_active_query_node_%s gauge\n",
			suffix, help, suffix);
	}
}

static void
buffer_ensure_capacity(FormatterBuffer *buf, u64 needed)
{
	if (unlikely(buf->offset + needed >= buf->size))
	{
		buf->size *= 2;
		buf->data = (char *) repalloc(buf->data, buf->size);
	}
}

static void
buffer_append(FormatterBuffer *buf, const char *fmt, ...)
{
	va_list args;
	i32		written;

	buffer_ensure_capacity(buf, 1024);

	va_start(args, fmt);
	written =
		vsnprintf(buf->data + buf->offset, buf->size - buf->offset, fmt, args);
	va_end(args);

	if (written > 0)
		buf->offset += written;
}

static void
formatter_init(Formatter *fmt, FormatterBuffer *buf, i32 pid, u64 query_id)
{
	fmt->buffer = buf;
	fmt->pid = pid;
	fmt->query_id = query_id;
}

static char *
discard_buffer(FormatterBuffer *buf)
{
	return buf->data;
}

static void
append_metric(Formatter *fmt, PwhNode *node, MetricType type,
			  const char *value_fmt, ...)
{
	va_list		args;
	const char *suffix = metric_suffix(type);
	const char *tag_str = pwh_node_tag_to_string(node->tag);
	char		value_buf[256];

	va_start(args, value_fmt);
	vsnprintf(value_buf, sizeof(value_buf), value_fmt, args);
	va_end(args);

	buffer_append(fmt->buffer,
				  "pg_what_is_happening_active_query_node_%s{pid=\"%d\","
				  "query_id=\"%lu\",node_id=\"%lu\",node_tag=\"%s\"} %s\n",
				  suffix, fmt->pid, fmt->query_id, node->node_id, tag_str,
				  value_buf);
}

static char *
format_openmetrics(void)
{
	FormatterBuffer buf;
	buffer_init(&buf);

	for (u64 i = 0; i < pwh_get_backend_entry_count(); i++)
	{
		PwhSharedMemoryBackendEntry *shmem_be_entry = pwh_get_backend_entry(i);

		if (!shmem_be_entry || shmem_be_entry->backend_pid == 0)
			continue;

		elog(DEBUG2,
			 "PWH: Formatting backend entry %d "
			 "PID=%d active=%d query_id=%lu num_nodes=%d",
			 i, shmem_be_entry->backend_pid, shmem_be_entry->is_query_active,
			 (unsigned long) shmem_be_entry->query_id,
			 shmem_be_entry->num_nodes);

		if (!shmem_be_entry->is_query_active)
			continue;

		PwhNode *plan_nodes = pwh_get_entry_plan_nodes(shmem_be_entry);

		Formatter fmt;
		formatter_init(&fmt, &buf, shmem_be_entry->backend_pid,
					   shmem_be_entry->query_id);

		double total_query_time = 0.0;
		for (u32 j = 0; j < shmem_be_entry->num_nodes; j++)
		{
			total_query_time += plan_nodes[j].execution.total_time_us;
		}

		for (u32 j = 0; j < shmem_be_entry->num_nodes; j++)
		{
			PwhNode *node = &plan_nodes[j];

			double node_time_seconds =
				node->execution.total_time_us / 1000000.0;
			double node_percent =
				(total_query_time > 0.0)
					? (node->execution.total_time_us / total_query_time) * 100.0
					: 0.0;

			append_metric(&fmt, node, METRIC_ROWS, "%.0f",
						  node->execution.tuples_returned);
			append_metric(&fmt, node, METRIC_TIME_SECONDS, "%.6f",
						  node_time_seconds);
			append_metric(&fmt, node, METRIC_TIME_PERCENT, "%.2f",
						  node_percent);
			append_metric(&fmt, node, METRIC_CACHE_HITS, "%ld",
						  (long) node->buffer_usage.cache_hits);
			append_metric(&fmt, node, METRIC_CACHE_MISSES, "%ld",
						  (long) node->buffer_usage.cache_misses);
			append_metric(&fmt, node, METRIC_LOCAL_CACHE_HITS, "%ld",
						  (long) node->buffer_usage.local_cache_hits);
			append_metric(&fmt, node, METRIC_LOCAL_CACHE_MISSES, "%ld",
						  (long) node->buffer_usage.local_cache_misses);
			append_metric(&fmt, node, METRIC_SPILL_FILE_READS, "%ld",
						  (long) node->buffer_usage.spill_file_reads);
			append_metric(&fmt, node, METRIC_SPILL_FILE_WRITES, "%ld",
						  (long) node->buffer_usage.spill_file_writes);
		}
	}

	return discard_buffer(&buf);
}

static void
handle_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;

	if (PWH_SERVER != NULL)
	{
		pwh_http_server_stop(PWH_SERVER);
	}

	proc_exit(0);
	errno = save_errno;
}
