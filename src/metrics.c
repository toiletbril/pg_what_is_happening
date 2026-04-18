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

#include "metrics.h"

#include <stdarg.h>
#include <stdio.h>

#include "catalog/pg_type.h"
#include "common.h"
#include "compatibility.h"
#include "funcapi.h"
#include "gucs.h"
#include "shared_memory.h"
#include "utils/builtins.h"

#define METRIC_BUFFER_SIZE 65536

typedef struct
{
	char *data;
	u64	  offset;
	u64	  size;
} FormatterBuffer;

typedef struct
{
	FormatterBuffer *buffer;
	u64				 query_id;
} Formatter;

static void buffer_init(FormatterBuffer *buf);
static void buffer_ensure_capacity(FormatterBuffer *buf, u64 needed);
static void buffer_append(FormatterBuffer *buf, const char *fmt, ...);
static void buffer_append_escaped(FormatterBuffer *buf, const char *str);
static void formatter_init(Formatter *fmt, FormatterBuffer *buf, u64 query_id);
static void formatter_append_metric(Formatter *fmt, PwhNodeMetrics *node,
									MetricType type, const char *value_fmt,
									...);
static void formatter_append_all_node_metrics(Formatter		 *fmt,
											  PwhNodeMetrics *node,
											  double		  total_query_time);
static void formatter_append_query_info(FormatterBuffer				*buf,
										PwhSharedMemoryBackendEntry *entry);

#define TupleDescInitEntryMetric(desc, att_num, metric_enum, type) \
	TupleDescInitEntry(desc, att_num, metric_suffix(metric_enum), type, -1, 0);


TupleDesc
pwh_create_v1_status_tupdesc(void)
{
	AttrNumber n = 1;
	TupleDesc  d = PWH_CREATE_TUPLE_DESC(PWH_V1_STATUS_TUPLE_COUNT);

	/* Utility columns. */
	TupleDescInitEntry(d, n++, "backend_pid", INT4OID, -1, 0);
	TupleDescInitEntry(d, n++, "query_id", INT8OID, -1, 0);
	TupleDescInitEntry(d, n++, "query_text", TEXTOID, -1, 0);
	TupleDescInitEntry(d, n++, "node_id", INT4OID, -1, 0);
	TupleDescInitEntry(d, n++, "parent_node_id", INT4OID, -1, 0);
	TupleDescInitEntry(d, n++, "node_tag", TEXTOID, -1, 0);

	/* Metrics. */
	TupleDescInitEntryMetric(d, n++, METRIC_STARTUP_TIME_US, FLOAT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_TOTAL_TIME_US, FLOAT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_LOOPS_EXECUTED, FLOAT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_TUPLES_RETURNED, FLOAT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_TIME_SECONDS, FLOAT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_TIME_PERCENT, FLOAT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_CACHE_HITS, INT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_CACHE_MISSES, INT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_LOCAL_CACHE_HITS, INT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_LOCAL_CACHE_MISSES, INT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_SPILL_FILE_READS, INT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_SPILL_FILE_WRITES, INT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_ROWS_FILTERED_BY_JOINS, FLOAT8OID);
	TupleDescInitEntryMetric(d, n++, METRIC_ROWS_FILTERED_BY_EXPRESSIONS,
							 FLOAT8OID);

	/* Column indexing starts from one. */
	Assert(n == PWH_V1_STATUS_TUPLE_COUNT + 1);

	return d;
}

void
pwh_fill_v1_status_tuple(Datum *values, bool *nulls,
						 PwhSharedMemoryBackendEntry *entry,
						 PwhNodeMetrics *node, double total_query_time)
{
	double node_time_seconds = node->execution.total_time_us / 1000000.0;
	double node_percent =
		(total_query_time > 0.0)
			? (node->execution.total_time_us / total_query_time) * 100.0
			: 0.0;

	u64 n = 0;

	MemSet(nulls, 0, PWH_V1_STATUS_TUPLE_COUNT * sizeof(bool));

	values[n++] = Int32GetDatum(entry->backend_pid);
	values[n++] = Int64GetDatum(entry->query_id);
	values[n++] = CStringGetTextDatum(pwh_get_backend_entry_query_text(entry));
	values[n++] = Int32GetDatum(node->node_id);
	values[n++] = Int32GetDatum(node->parent_node_id);
	values[n++] = CStringGetTextDatum(pwh_node_tag_to_string(node->tag));
	values[n++] = Float8GetDatum(node->execution.startup_time_us);
	values[n++] = Float8GetDatum(node->execution.total_time_us);
	values[n++] = Float8GetDatum(node->execution.loops_executed);
	values[n++] = Float8GetDatum(node->execution.tuples_returned);
	values[n++] = Float8GetDatum(node_time_seconds);
	values[n++] = Float8GetDatum(node_percent);
	values[n++] = Int64GetDatum(node->buffer_usage.cache_hits);
	values[n++] = Int64GetDatum(node->buffer_usage.cache_misses);
	values[n++] = Int64GetDatum(node->buffer_usage.local_cache_hits);
	values[n++] = Int64GetDatum(node->buffer_usage.local_cache_misses);
	values[n++] = Int64GetDatum(node->buffer_usage.spill_file_reads);
	values[n++] = Int64GetDatum(node->buffer_usage.spill_file_writes);
	values[n++] = Float8GetDatum(node->execution.rows_filtered_by_joins);
	values[n++] = Float8GetDatum(node->execution.rows_filtered_by_expressions);

	Assert(n == PWH_V1_STATUS_TUPLE_COUNT);
}

static void
formatter_append_all_node_metrics(Formatter *fmt, PwhNodeMetrics *node,
								  double total_query_time)
{
	double node_time_seconds = node->execution.total_time_us / 1000000.0;
	double node_percent =
		(total_query_time > 0.0)
			? (node->execution.total_time_us / total_query_time) * 100.0
			: 0.0;

	formatter_append_metric(fmt, node, METRIC_TUPLES_RETURNED, "%.0f",
							node->execution.tuples_returned);
	formatter_append_metric(fmt, node, METRIC_STARTUP_TIME_US, "%.0f",
							node->execution.startup_time_us);
	formatter_append_metric(fmt, node, METRIC_TOTAL_TIME_US, "%.0f",
							node->execution.total_time_us);
	formatter_append_metric(fmt, node, METRIC_LOOPS_EXECUTED, "%.0f",
							node->execution.loops_executed);
	formatter_append_metric(fmt, node, METRIC_TIME_SECONDS, "%.6f",
							node_time_seconds);
	formatter_append_metric(fmt, node, METRIC_TIME_PERCENT, "%.2f",
							node_percent);
	formatter_append_metric(fmt, node, METRIC_CACHE_HITS, "%ld",
							(long) node->buffer_usage.cache_hits);
	formatter_append_metric(fmt, node, METRIC_CACHE_MISSES, "%ld",
							(long) node->buffer_usage.cache_misses);
	formatter_append_metric(fmt, node, METRIC_LOCAL_CACHE_HITS, "%ld",
							(long) node->buffer_usage.local_cache_hits);
	formatter_append_metric(fmt, node, METRIC_LOCAL_CACHE_MISSES, "%ld",
							(long) node->buffer_usage.local_cache_misses);
	formatter_append_metric(fmt, node, METRIC_SPILL_FILE_READS, "%ld",
							(long) node->buffer_usage.spill_file_reads);
	formatter_append_metric(fmt, node, METRIC_SPILL_FILE_WRITES, "%ld",
							(long) node->buffer_usage.spill_file_writes);
	formatter_append_metric(fmt, node, METRIC_ROWS_FILTERED_BY_JOINS, "%.0f",
							node->execution.rows_filtered_by_joins);
	formatter_append_metric(fmt, node, METRIC_ROWS_FILTERED_BY_EXPRESSIONS,
							"%.0f",
							node->execution.rows_filtered_by_expressions);
}


static void
buffer_init(FormatterBuffer *buf)
{
	buf->offset = 0;
	buf->size = METRIC_BUFFER_SIZE;
	buf->data = (char *) palloc(buf->size);
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
	{
		buf->offset += written;
	}
}

static void
formatter_init(Formatter *fmt, FormatterBuffer *buf, u64 query_id)
{
	fmt->buffer = buf;
	fmt->query_id = query_id;
}

static void
formatter_append_metric(Formatter *fmt, PwhNodeMetrics *node, MetricType type,
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
				  "pg_what_is_happening_active_query_node_%s{"
				  "query_id=\"%lu\",node_id=\"%lu\","
				  "parent_node_id=\"%d\",node_tag=\"%s\"} %s\n",
				  suffix, fmt->query_id, node->node_id,
				  (i32) node->parent_node_id, tag_str, value_buf);
}

char *
pwh_format_openmetrics(void)
{
	FormatterBuffer buf;
	buffer_init(&buf);

	/* Add query_info metric help and type. */
	buffer_append(&buf,
				  "# HELP pg_what_is_happening_query_info "
				  "Query metadata for active queries\n"
				  "# TYPE pg_what_is_happening_query_info gauge\n");

	/* Initialize help for node metrics. */
	for (u32 i = METRIC_START; i < METRIC_COUNT; i++)
	{
		const char *suffix = metric_suffix((MetricType) i);
		const char *help = metric_help((MetricType) i);

		buffer_append(
			&buf,
			"# HELP pg_what_is_happening_active_query_node_%s %s\n"
			"# TYPE pg_what_is_happening_active_query_node_%s gauge\n",
			suffix, help, suffix);
	}

	for (u64 i = 0; i < (u64) PWH_GUC_MAX_TRACKED_QUERIES; i++)
	{
		PwhSharedMemoryBackendEntry *shmem_be_entry = pwh_get_backend_entry(i);

		if (!pwh_is_backend_entry_active(shmem_be_entry))
			continue;

		ereport(DEBUG2, (errmsg("PWH: Formatting backend entry %lu", i),
						 errdetail("PID=%d query_id=%lu num_nodes=%d",
								   shmem_be_entry->backend_pid,
								   (unsigned long) shmem_be_entry->query_id,
								   shmem_be_entry->count_of_metrics)));

		/* _info pseudo-metric. */
		formatter_append_query_info(&buf, shmem_be_entry);

		Formatter fmt;
		formatter_init(&fmt, &buf, shmem_be_entry->query_id);

		PwhNodeMetrics *metrics = pwh_get_backend_entry_metrics(shmem_be_entry);

		double total_query_time = 0.0;
		for (u32 j = 0; j < shmem_be_entry->count_of_metrics; j++)
		{
			total_query_time += metrics[j].execution.total_time_us;
		}

		for (u32 j = 0; j < shmem_be_entry->count_of_metrics; j++)
		{
			/* Validate node magic before reading. */
			if (!pwh_validate_node_magic(&metrics[j], j))
				continue;

			formatter_append_all_node_metrics(&fmt, &metrics[j],
											  total_query_time);
		}
	}

	return buf.data;
}

static void
buffer_append_escaped(FormatterBuffer *buf, const char *str)
{
	const char *p = str;

	while (*p != '\0')
	{
		switch (*p)
		{
			case '"':
				buffer_append(buf, "\\\"");
				break;
			case '\\':
				buffer_append(buf, "\\\\");
				break;
			case '\n':
				buffer_append(buf, "\\n");
				break;
			default:
				buffer_ensure_capacity(buf, 1);
				buf->data[buf->offset++] = *p;
				break;
		}
		p++;
	}
}

static void
formatter_append_query_info(FormatterBuffer				*buf,
							PwhSharedMemoryBackendEntry *entry)
{
	buffer_append(buf,
				  "pg_what_is_happening_query_info{query_id=\"%lu\","
				  "pid=\"%d\",query_text=\"",
				  entry->query_id, entry->backend_pid);
	const char *query_text = pwh_get_backend_entry_query_text(entry);
	buffer_append_escaped(buf, query_text);
	buffer_append(buf, "\"} 1\n");
}
