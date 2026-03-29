# pg_what_is_happening

Live per-plan-node execution OpenMetrics-compatible stats for PostgreSQL and
friends.

If you use this in production you're braver than I am. The rest of the
description is work-in-progress.

## Development

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

## Usage

Direct query:
```sql
SELECT * FROM what_is_happening.v1_status;
```

Exposed `/metrics` via background worker (`WITH_BGWORKER=yes`):
```bash
$ curl localhost:9187/metrics
```

Example prometheus queries:
```sql
# Top 5 slowest nodes right now
topk(5, pg_what_is_happening_active_query_node_time_seconds)

# Nodes spilling to disk
pg_what_is_happening_active_query_node_temp_written > 0

# Cache hit ratio by node type
sum by (node_tag) (pg_what_is_happening_active_query_node_shared_hit)
/
(sum by (node_tag) (pg_what_is_happening_active_query_node_shared_hit + pg_what_is_happening_active_query_node_shared_read))
```
