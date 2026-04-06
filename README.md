# pg_what_is_happening

[![Regression Test Suite](https://github.com/toiletbril/pg_what_is_happening/actions/workflows/pg-regress.yml/badge.svg?branch=master)](https://github.com/toiletbril/pg_what_is_happening/actions/workflows/pg-regress.yml)

Live per-plan-node execution OpenMetrics-compatible stats for PostgreSQL and
friends with the most granular per-node visibility the executor can provide.
There are tests, but if you currently use this in production you're braver than
I am.

PostgreSQL versions from 9.4 through 18+ are supported. No stable ABI for `.so`
is guaranteed until stated otherwise.

See [`example/`](./example/) for a Prometheus/Grafana setup.

## Usage

The extension exposes metrics through two interfaces: a SQL view for direct
queries and an `/metrics` HTTP endpoint for your monitoring stack.

### SQL view

Query the `what_is_happening.v1_status` view to see live metrics for all active
queries across backends. Each row represents one plan node from one query, with
timing, tuple counts, and buffer usage stats. This view is backed by the
`what_is_happening.v1_status()` function which polls all backends and
aggregates their current execution state into a result set.

```sql
SELECT * FROM what_is_happening.v1_status;
```

#### Available metrics

| Column                         | Type     | Description                                                  |
|--------------------------------|----------|--------------------------------------------------------------|
|  Identification                |          |                                                              |
| `backend_pid`                  | `int4`   | Process ID of the backend executing the query.               |
| `query_id`                     | `int8`   | Unique identifier for the query.                             |
| `query_text`                   | `text`   | The SQL query text (truncated to `query_text_len`).          |
| `node_id`                      | `int4`   | Sequential ID of this plan node in the tree.                 |
| `parent_node_id`               | `int4`   | ID of the parent node in the plan tree.                      |
| `node_tag`                     | `text`   | PostgreSQL plan node type (e.g., `SeqScan`, `HashJoin`).     |
|  Execution metrics             |          |                                                              |
| `startup_time_us`              | `float8` | Time to produce the first tuple in microseconds.             |
| `total_time_us`                | `float8` | Total execution time in microseconds.                        |
| `loops_executed`               | `float8` | Number of times this plan node was executed.                 |
| `tuples_returned`              | `float8` | Number of tuples returned by this plan node.                 |
| `time_seconds`                 | `float8` | Execution time in seconds.                                   |
| `time_percent`                 | `float8` | Percentage of total query time spent in this node.           |
| `rows_filtered_by_joins`       | `float8` | Rows removed by scan or join conditions.                     |
| `rows_filtered_by_expressions` | `float8` | Rows removed by other filter expressions.                    |
| Buffer metrics                 |          |                                                              |
| `cache_hits`                   | `int8`   | Shared buffer cache hits.                                    |
| `cache_misses`                 | `int8`   | Shared buffer cache misses (blocks read from disk).          |
| `local_cache_hits`             | `int8`   | Local buffer cache hits.                                     |
| `local_cache_misses`           | `int8`   | Local buffer cache misses.                                   |
| `spill_file_reads`             | `int8`   | Blocks read from temporary spill files.                      |
| `spill_file_writes`            | `int8`   | Blocks written to temporary spill files.                     |

### HTTP metrics endpoint

When compiled with `WITH_BGWORKER=yes` (the default), the extension starts a
background worker that listens on `what_is_happening.metrics_listen_address`` by
default. Hit `/metrics` for [OpenMetrics](https://openmetrics.io/) formatted
output compatible with Prometheus, VictoriaMetrics, or et cetera.

```bash
$ curl localhost:9187/metrics
```

The HTTP endpoint exposes the same execution and buffer usage metrics from the
SQL view, prefixed with `pg_what_is_happening_active_query_node_`. Each metric
includes labels for `query_id`, `node_id`, and `node_tag`.

Additionally, the endpoint exposes a `pg_what_is_happening_query_info` metric
with labels `query_id`, `pid`, and `query_text`. The value is always `1`. This
info metric allows correlating query_id to metadata like PID and SQL text in
PromQL using `group_left`:

```promql
# Get slowest nodes with query text and PID attached
topk(10,
  pg_what_is_happening_active_query_node_time_seconds
    * on(query_id) group_left(query_text, pid)
    pg_what_is_happening_query_info
)
```

More example queries:

```promql
# Top 10 slowest nodes right now
topk(10, pg_what_is_happening_active_query_node_time_seconds)

# Nodes spilling to disk
pg_what_is_happening_active_query_node_spill_file_writes > 0

# Cache hit ratio by node type
sum by (node_tag) (pg_what_is_happening_active_query_node_cache_hits)
/
sum by (node_tag) (
  pg_what_is_happening_active_query_node_cache_hits +
  pg_what_is_happening_active_query_node_cache_misses
)

# All currently tracked queries with their text
pg_what_is_happening_query_info
```

#### Cardinality and performance

The extension is designed for real-time monitoring of currently running queries
only. Metrics only exist while queries are active and disappear when they
complete.

Cardinality is bounded by:

$$ \text{max_tracked_queries} \times \text{max_nodes_per_query} \times \text{metric_count} $$

With defaults:

$$ 32 \times 128 \times 14 \approx 57000 \text{ active_series_maximum} $$

Actual cardinality is typically much lower as most queries have fewer nodes.

Each unique `query_id` creates new time series. The extension does not
normalize queries like `pg_stat_statements` does. Use `min_cost_to_track` to
filter out cheap queries and reduce cardinality on busy systems.

This extension is not suitable for long-term trend analysis or query pattern
aggregation. Use `pg_stat_statements` for that. The `/metrics` endpoint exists
to answer the question "WHAT IS SLOW RIGHT NOW" with the most granular per-node
visibility the Postgres executor can provide.

## Settings

| Setting                                    | Default          | Sensible range   | Reload   | Description                                                                                         |
|--------------------------------------------|------------------|------------------|----------|-----------------------------------------------------------------------------------------------------|
| `what_is_happening.is_enabled`             | `true`           | —                | `SIGHUP` | Enable or disable the extension without unloading it.                                               |
| `what_is_happening.metrics_listen_address` | `127.0.0.1:9187` | —                | Restart  | Address and port for the `/metrics` HTTP endpoint. Only available if compiled with `WITH_BGWORKER`. |
| `what_is_happening.max_tracked_queries`    | `32`             | 2–256            | Restart  | Number of concurrent query slots allocated in shared memory. Each slot holds one backend's metrics. |
| `what_is_happening.max_nodes_per_query`    | `128`            | 16–256           | Restart  | Maximum plan nodes tracked per query. Plans with more nodes get truncated.                          |
| `what_is_happening.max_query_text_length`  | `1024`           | 64–8192          | Restart  | Maximum bytes of query text stored. Longer queries get truncated.                                   |
| `what_is_happening.signal_timeout_ms`      | `32`             | 8–10000          | `SIGHUP` | How long to wait for each backend to respond to metrics requests before giving up (milliseconds).   |
| `what_is_happening.min_cost_to_track`      | `50000.0`        | 0.0-inf          | `SIGHUP` | Minimum total cost of a query to get tracked by the extension.                                      |

## Performance

Shared memory consumption is

$$
\text{header} + (\text{max_tracked_queries}
\times \text{backend_entry_stride})
$$

where the backend entry stride is

$$ \approx 40 + \text{query_text_len} + (\text{max_nodes_per_query} \times 120) $$

With default settings (`max_tracked_queries=32`,
`max_nodes_per_query=128`, `max_query_text_length=1024`):

$$ 16 + (32 \times (40 + 1024 + 15360)) = 525584 \text{ bytes} \approx 513 \text{ KB} $$

Higher settings scale linearly:
`queries=256`, `nodes=256`, `text=2048` is around 8 MB.
`queries=1024`, `nodes=1024`, `text=4096` is around 126 MB.

# Development

[`Dockerfile`](./Dockerfile) is used as a base image for integration tests.
Take a look inside [`Shfile.sh`](./Shfile.sh) for more context.

`master` holds stable, tested versions. PRs targeting master trigger the full
regression test suite.
`staging` is the trunk branch and can break at any time during active
development.

## Building

```bash
$ POSTGRES_SOURCE='<path/to/postgres/source>' ./Shfile.sh make-image
$ POSTGRES_SOURCE='<path/to/postgres/source>' ./Shfile.sh build
# or, without containers, requires PXGS:
$ make -j$(nproc)
# then, install, if the current host is your postgres machine:
$ make install
# otherwise put the static .so and utility files to your instance manually.
```

Happy hacking!
