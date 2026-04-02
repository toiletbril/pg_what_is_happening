# pg_what_is_happening monitoring example

You need a PostgresSQL instance running on `localhost`, with
`pg_what_is_happening` extension created. Run `Shfile.sh dev` load the
extension yourself with `CREATE EXTENSION pg_what_is_happening`.

Starting:
```
$ docker-compose up -d
```

Grafana runs on `localhost:3000`. Prometheus runs on `localhost:9090` and
scrapes `localhost:9187/metrics` every second.

The dashboard tracks active query count, per-node execution time, cache hit
ratios, rows produced, node type distribution, and a live table of running
queries.

Metrics use labels for `pid`, `query_id`, `node_id`, and `node_tag`.

Configuration:

The extension serves metrics on `localhost:9187` by default. Change via
`what_is_happening.listen_address` GUC in `postgresql.conf` or at runtime:
```
ALTER SYSTEM SET what_is_happening.listen_address = '0.0.0.0:9187';
SELECT pg_reload_conf();
```

Keep it on localhost unless remote access is needed and firewalled. Update
`prometheus/prometheus.yml` if the endpoint changes.

Scrape interval is 1 second. Adjust in `prometheus.yml` if needed.

Available metrics:

All prefixed with `pg_what_is_happening_active_query_node_`.

- `rows` - tuples produced by node
- `time_seconds` - execution time
- `time_percent` - percentage of total query time
- `cache_hits` - shared buffer hits
- `cache_misses` - shared buffer misses
- `local_cache_hits` - local buffer hits
- `local_cache_misses` - local buffer misses
- `spill_file_reads` - blocks read from spill files
- `spill_file_writes` - blocks written to spill files

Find nodes spilling to disk:

```sql
pg_what_is_happening_active_query_node_spill_file_writes > 0
```

Import to existing Grafana:

Copy `grafana/provisioning/dashboards/pg_what_is_happening.json` and import via
`Dashboards` -> `Import`.
