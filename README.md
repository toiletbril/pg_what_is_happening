# pg_what_is_happening

[![Regression Test Suite](https://github.com/toiletbril/pg_what_is_happening/actions/workflows/pg-regress.yml/badge.svg?branch=master)](https://github.com/toiletbril/pg_what_is_happening/actions/workflows/pg-regress.yml)

Live per-plan-node execution OpenMetrics-compatible stats for PostgreSQL and
friends. There are tests, but if you currently use this in production you're
braver than I am.

PostgreSQL versions from 9.4 through 18+ are supported with varying levels of
jank in the compatibility layer.

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
| `backend_pid`                  | `int4`   | Process ID of the backend executing the query.               |
| `query_id`                     | `int8`   | Unique identifier for the query.                             |
| `query_text`                   | `text`   | The SQL query text (truncated to `query_text_len`).          |
| `node_id`                      | `int4`   | Sequential ID of this plan node in the tree.                 |
| `parent_node_id`               | `int4`   | ID of the parent node in the plan tree.                      |
| `node_tag`                     | `text`   | PostgreSQL plan node type (e.g., `SeqScan`, `HashJoin`).     |
|                                |          |                                                              |
| `tuples_returned`              | `float8` | Number of tuples returned by this plan node.                 |
| `startup_time_us`              | `float8` | Time to produce the first tuple in microseconds.             |
| `total_time_us`                | `float8` | Total execution time in microseconds.                        |
| `loops_executed`               | `float8` | Number of times this plan node was executed.                 |
| `rows`                         | `float8` | Rows produced by this plan node (same as `tuples_returned`). |
| `time_seconds`                 | `float8` | Execution time in seconds.                                   |
| `time_percent`                 | `float8` | Percentage of total query time spent in this node.           |
| `rows_filtered_by_joins`       | `float8` | Rows removed by scan or join conditions.                     |
| `rows_filtered_by_expressions` | `float8` | Rows removed by other filter expressions.                    |
|                                |          |                                                              |
| `cache_hits`                   | `int8`   | Shared buffer cache hits.                                    |
| `cache_misses`                 | `int8`   | Shared buffer cache misses (blocks read from disk).          |
| `local_cache_hits`             | `int8`   | Local buffer cache hits.                                     |
| `local_cache_misses`           | `int8`   | Local buffer cache misses.                                   |
| `spill_file_reads`             | `int8`   | Blocks read from temporary spill files.                      |
| `spill_file_writes`            | `int8`   | Blocks written to temporary spill files.                     |

### HTTP metrics endpoint

When compiled with `WITH_BGWORKER=yes` (the default), the extension starts a
background worker that listens on `127.0.0.1:9187` by default. Hit `/metrics`
for [OpenMetrics](https://openmetrics.io/) formatted output compatible with
Prometheus, VictoriaMetrics, or et cetera.

```bash
$ curl localhost:9187/metrics
```

Point your Prometheus at this endpoint and you'll have per-node execution
visibility in Grafana dashboards with maximum rizz to flex your friends which
nodes are spilling to disk, cache hit ratios, or easily find the slowest joins.

Example PromQL queries for your dashboards:

```sql
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
```

## Settings

| Setting                                 | Default          | Range   | Reload   | Description                                                                                         |
|-----------------------------------------|------------------|---------|----------|-----------------------------------------------------------------------------------------------------|
| `what_is_happening.enabled`             | `true`           | —       | `SIGHUP` | Enable or disable the extension without unloading it.                                               |
| `what_is_happening.signal_timeout_ms`   | `10`             | 1–1000  | `SIGHUP` | How long to wait for each backend to respond to metrics requests before giving up (milliseconds).   |
| `what_is_happening.listen_address`      | `127.0.0.1:9187` | —       | Restart  | Address and port for the `/metrics` HTTP endpoint. Only available if compiled with `WITH_BGWORKER`. |
| `what_is_happening.max_tracked_queries` | `128`            | 1–65536 | Restart  | Number of concurrent query slots allocated in shared memory. Each slot holds one backend's metrics. |
| `what_is_happening.max_nodes_per_query` | `128`            | 16–1024 | Restart  | Maximum plan nodes tracked per query. Plans with more nodes get truncated.                          |
| `what_is_happening.query_text_len`      | `1024`           | 64–8192 | Restart  | Maximum bytes of query text stored. Longer queries get truncated.                                   |

## Performance

### Memory usage

Shared memory consumption is
```
(header + per_backend_entry * max_tracked_queries)
```
where each backend entry is roughly
```
48 bytes + query_text_len + (max_nodes_per_query * 96 bytes)
```
and with default settings, that's about
```
48 + 1024 + 12288 = 13.3KB per slot * 128 slots = 1.7MB total.
```

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
