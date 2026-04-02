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
	u32				 pid;
	u64				 query_id;
} Formatter;

static void buffer_init(FormatterBuffer *buf);
static void buffer_ensure_capacity(FormatterBuffer *buf, u64 needed);
static void buffer_append(FormatterBuffer *buf, const char *fmt, ...);
static void formatter_init(Formatter *fmt, FormatterBuffer *buf, u32 pid,
						   u64 query_id);
static void append_metric(Formatter *fmt, PwhNodeMetrics *node, MetricType type,
						  const char *value_fmt, ...);

TupleDesc
pwh_create_v1_status_tupdesc(void)
{
	TupleDesc tupdesc = PWH_CREATE_TUPLE_DESC(PWH_V1_STATUS_TUPLE_COUNT);

	/* Utility columns. */
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "backend_pid", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "query_id", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "query_text", TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "node_id", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "parent_node_id", INT4OID, -1,
					   0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "node_tag", TEXTOID, -1, 0);

	/* Metrics. */
	TupleDescInitEntry(tupdesc, (AttrNumber) 7,
					   metric_suffix(METRIC_STARTUP_TIME_US), FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 8,
					   metric_suffix(METRIC_TOTAL_TIME_US), FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 9,
					   metric_suffix(METRIC_LOOPS_EXECUTED), FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 10, metric_suffix(METRIC_ROWS),
					   FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 11,
					   metric_suffix(METRIC_TIME_SECONDS), FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 12,
					   metric_suffix(METRIC_TIME_PERCENT), FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 13,
					   metric_suffix(METRIC_CACHE_HITS), INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 14,
					   metric_suffix(METRIC_CACHE_MISSES), INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 15,
					   metric_suffix(METRIC_LOCAL_CACHE_HITS), INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 16,
					   metric_suffix(METRIC_LOCAL_CACHE_MISSES), INT8OID, -1,
					   0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 17,
					   metric_suffix(METRIC_SPILL_FILE_READS), INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 18,
					   metric_suffix(METRIC_SPILL_FILE_WRITES), INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 19,
					   metric_suffix(METRIC_ROWS_FILTERED_BY_JOINS), FLOAT8OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 20,
					   metric_suffix(METRIC_ROWS_FILTERED_BY_EXPRESSIONS),
					   FLOAT8OID, -1, 0);

	return tupdesc;
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


	MemSet(nulls, 0, PWH_V1_STATUS_TUPLE_COUNT * sizeof(bool));

	values[0] = Int32GetDatum(entry->backend_pid);
	values[1] = Int64GetDatum(entry->query_id);
	values[2] = CStringGetTextDatum(pwh_get_backend_entry_query_text(entry));

	values[3] = Int32GetDatum(node->node_id);
	values[4] = Int32GetDatum(node->parent_node_id);
	values[5] = CStringGetTextDatum(pwh_node_tag_to_string(node->tag));
	values[6] = Float8GetDatum(node->execution.startup_time_us);
	values[7] = Float8GetDatum(node->execution.total_time_us);
	values[8] = Float8GetDatum(node->execution.loops_executed);
	values[9] = Float8GetDatum(node->execution.tuples_returned);
	values[10] = Float8GetDatum(node_time_seconds);
	values[11] = Float8GetDatum(node_percent);
	values[12] = Int64GetDatum(node->buffer_usage.cache_hits);
	values[13] = Int64GetDatum(node->buffer_usage.cache_misses);
	values[14] = Int64GetDatum(node->buffer_usage.local_cache_hits);
	values[15] = Int64GetDatum(node->buffer_usage.local_cache_misses);
	values[16] = Int64GetDatum(node->buffer_usage.spill_file_reads);
	values[17] = Int64GetDatum(node->buffer_usage.spill_file_writes);
	values[18] = Float8GetDatum(node->execution.rows_filtered_by_joins);
	values[19] = Float8GetDatum(node->execution.rows_filtered_by_expressions);
}

static void
append_all_node_metrics(Formatter *fmt, PwhNodeMetrics *node,
						double total_query_time)
{
	double node_time_seconds = node->execution.total_time_us / 1000000.0;
	double node_percent =
		(total_query_time > 0.0)
			? (node->execution.total_time_us / total_query_time) * 100.0
			: 0.0;

	append_metric(fmt, node, METRIC_ROWS, "%.0f",
				  node->execution.tuples_returned);
	append_metric(fmt, node, METRIC_TIME_SECONDS, "%.6f", node_time_seconds);
	append_metric(fmt, node, METRIC_TIME_PERCENT, "%.2f", node_percent);
	append_metric(fmt, node, METRIC_CACHE_HITS, "%ld",
				  (long) node->buffer_usage.cache_hits);
	append_metric(fmt, node, METRIC_CACHE_MISSES, "%ld",
				  (long) node->buffer_usage.cache_misses);
	append_metric(fmt, node, METRIC_LOCAL_CACHE_HITS, "%ld",
				  (long) node->buffer_usage.local_cache_hits);
	append_metric(fmt, node, METRIC_LOCAL_CACHE_MISSES, "%ld",
				  (long) node->buffer_usage.local_cache_misses);
	append_metric(fmt, node, METRIC_SPILL_FILE_READS, "%ld",
				  (long) node->buffer_usage.spill_file_reads);
	append_metric(fmt, node, METRIC_SPILL_FILE_WRITES, "%ld",
				  (long) node->buffer_usage.spill_file_writes);
	append_metric(fmt, node, METRIC_ROWS_FILTERED_BY_JOINS, "%.0f",
				  node->execution.rows_filtered_by_joins);
	append_metric(fmt, node, METRIC_ROWS_FILTERED_BY_EXPRESSIONS, "%.0f",
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
		buf->offset += written;
}

static void
formatter_init(Formatter *fmt, FormatterBuffer *buf, u32 pid, u64 query_id)
{
	fmt->buffer = buf;
	fmt->pid = pid;
	fmt->query_id = query_id;
}

static void
append_metric(Formatter *fmt, PwhNodeMetrics *node, MetricType type,
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

char *
pwh_format_openmetrics(void)
{
	FormatterBuffer buf;
	buffer_init(&buf);

	/* Initialize help. */
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

	for (u64 i = 0; i < pwh_get_backend_entry_count(); i++)
	{
		PwhSharedMemoryBackendEntry *shmem_be_entry = pwh_get_backend_entry(i);

		if (shmem_be_entry == NULL || shmem_be_entry->backend_pid == 0)
			continue;

		ereport(DEBUG2, (errmsg("PWH: Formatting backend entry %lu", i),
						 errdetail("PID=%d query_id=%lu num_nodes=%d",
								   shmem_be_entry->backend_pid,
								   (unsigned long) shmem_be_entry->query_id,
								   shmem_be_entry->num_nodes)));

		PwhNodeMetrics *metrics = pwh_get_backend_entry_metrics(shmem_be_entry);

		Formatter fmt;
		formatter_init(&fmt, &buf, shmem_be_entry->backend_pid,
					   shmem_be_entry->query_id);

		double total_query_time = 0.0;
		for (u32 j = 0; j < shmem_be_entry->num_nodes; j++)
		{
			total_query_time += metrics[j].execution.total_time_us;
		}

		for (u32 j = 0; j < shmem_be_entry->num_nodes; j++)
		{
			/* Validate node magic before reading. */
			if (!pwh_validate_node_magic(&metrics[j], j))
				continue;

			append_all_node_metrics(&fmt, &metrics[j], total_query_time);
		}
	}

	return buf.data;
}
