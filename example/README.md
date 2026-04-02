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

The extension serves metrics on `localhost:9187` by default. Update
`prometheus/prometheus.yml` if the endpoint changes. Scrape interval is 1
second, adjust in `prometheus.yml` if needed.

See the [main README](../README.md) for available metrics and configuration
options.

Find nodes spilling to disk:

```sql
pg_what_is_happening_active_query_node_spill_file_writes > 0
```

Import to existing Grafana:

Copy `grafana/provisioning/dashboards/pg_what_is_happening.json` and import via
`Dashboards` -> `Import`.
