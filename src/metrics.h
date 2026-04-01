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

#ifndef PWH_METRICS_H
#define PWH_METRICS_H

#include "postgres.h"

#include "common.h"
#include "funcapi.h"
#include "shared_memory.h"

typedef enum
{
	METRIC_START = 0,
	METRIC_TUPLES_RETURNED = METRIC_START,
	METRIC_STARTUP_TIME_US,
	METRIC_TOTAL_TIME_US,
	METRIC_LOOPS_EXECUTED,
	METRIC_ROWS,
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

#define METRIC_BUFFER_SIZE 65536

static forceinline const char *
metric_suffix(MetricType type)
{
	switch (type)
	{
		case METRIC_TUPLES_RETURNED:
			return "tuples_returned";
		case METRIC_STARTUP_TIME_US:
			return "startup_time_us";
		case METRIC_TOTAL_TIME_US:
			return "total_time_us";
		case METRIC_LOOPS_EXECUTED:
			return "loops_executed";
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

static forceinline const char *
metric_help(MetricType type)
{
	switch (type)
	{
		case METRIC_TUPLES_RETURNED:
			return "Number of tuples returned by plan node";
		case METRIC_STARTUP_TIME_US:
			return "Startup time in microseconds";
		case METRIC_TOTAL_TIME_US:
			return "Total execution time in microseconds";
		case METRIC_LOOPS_EXECUTED:
			return "Number of times plan node was executed";
		case METRIC_ROWS:
			return "Rows produced by active query plan node";
		case METRIC_TIME_SECONDS:
			return "Execution time for active query plan node in seconds";
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

TupleDesc pwh_create_metrics_tupdesc(void);
void	  pwh_fill_metrics_tuple(Datum *values, bool *nulls,
								 PwhSharedMemoryBackendEntry *entry,
								 PwhNodeMetrics *node, double total_query_time);
char	 *pwh_format_openmetrics(void);

#endif
