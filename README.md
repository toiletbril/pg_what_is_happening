# pg_what_is_happening

[![Regression Test Suite](https://github.com/toiletbril/pg_what_is_happening/actions/workflows/pg-regress.yml/badge.svg?branch=master)](https://github.com/toiletbril/pg_what_is_happening/actions/workflows/pg-regress.yml)

Live per-plan-node execution OpenMetrics-compatible stats for PostgreSQL and
friends. There are tests, but if you currently use this in production you're
braver than I am.

PostgreSQL versions from 9.4 through 18+ are supported with varying levels of
jank in the compatibility layer.

## Usage

The extension exposes metrics through two interfaces: a SQL view for direct
queries and an `/metrics` HTTP endpoint for your monitoring stack.

### SQL view

Query the `what_is_happening.v1_status` view to see live metrics for all active
queries across backends. Each row represents one plan node from one query, with
timing, tuple counts, and buffer usage stats. This view is backed by the
`what_is_happening.v1_status()` function which polls all backends via SIGUSR2
and aggregates their current execution state into a result set.

```sql
SELECT * FROM what_is_happening.v1_status;
```

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

- `what_is_happening.enabled` defaults to true. Set it to `false` if you want
  the extension loaded but dormant. Toggled with a `SIGHUP`

- `what_is_happening.signal_timeout_ms` defaults to 10ms, range is 1 to 1000.
  How long we wait for each backend to respond to request for metrics before
  giving up. Toggled with a `SIGHUP`.

- `what_is_happening.listen_address` defaults to `127.0.0.1:9187` and controls
  where the background worker binds for the `/metrics` HTTP endpoint. Only
  available if compiled `WITH_BGWORKER`. Requires restart.

- `what_is_happening.max_tracked_queries` defaults to 128, range is 1 to 65536.
  This is how many concurrent query slots is allocated in shared memory. Each
  slot can hold one backend's metrics. If you have more active backends than
  slots, some queries won't get tracked. Requires restart.

- `what_is_happening.max_nodes_per_query` defaults to 128, range is 16 to 1024.
  Maximum plan nodes tracked per query. If your query plan has 200 nodes but
  this is set to 128, you'll only see the first 128 nodes in the output.
  Requires restart.

- `what_is_happening.query_text_len` defaults to 1024, range is 64 to 8192. How
  many bytes of query text get stored. Longer queries get truncated. Requires
  restart.

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
