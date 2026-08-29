# pg_what_is_happening monitoring stack example

<!-- XXX make node graph actually useful -->

The example Grafana dashboard provides a live page of active queries, utilizing
most of the metrics provided by the extension.

See the [main README](../README.md) for available metrics and configuration
options.

## Usage

All containers are started in the host network.

You need a PostgresSQL instance running on `localhost`, with
`pg_what_is_happening` extension created. For development environments, run
`Shfile.sh dev`, which starts a PostgreSQL container in a host network too,
and load the extension yourself with `CREATE EXTENSION pg_what_is_happening`.

Compose includes pre-configured Grafana and Prometheus.

```bash
# starting:
$ docker compose up -d
# stopping:
$ docker compose down
```

Grafana runs on `localhost:3000`. Prometheus runs on `localhost:9090` and
scrapes the default address of `localhost:9187/metrics` (consult your
`what_is_happening.metrics_listen_address` GUC) every second.

## Import to existing Grafana:

Import [the dashboard](grafana/provisioning/dashboards/pg_what_is_happening.json)
via `Dashboards` -> `Import`.
