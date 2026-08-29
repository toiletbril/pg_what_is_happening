SELECT pg_advisory_lock(12351);

ALTER SYSTEM SET what_is_happening.signal_timeout_ms = 10000;
ALTER SYSTEM SET what_is_happening.sample_interval_ms = 100;
SELECT pg_reload_conf();
\! curl --silent --max-time 5 http://localhost:9187/metrics > /dev/null

\! PGAPPNAME=pwh_cache_freshness psql -X -v ON_ERROR_STOP=1 -d contrib_regression -c "SELECT pg_advisory_lock(12351), COUNT(*) FROM orders o JOIN users u ON o.user_id = u.user_id LIMIT 10; SELECT pg_advisory_unlock(12351);" > /dev/null 2>&1 &

SELECT pg_sleep(0.2);

\! curl --fail --silent --show-error --max-time 5 -D /tmp/pwh_openmetrics_headers.txt http://localhost:9187/metrics > /tmp/pwh_openmetrics.txt

\! pid=$(psql -X -At -d contrib_regression -c "SELECT pid FROM pg_stat_activity WHERE application_name = 'pwh_cache_freshness'"); grep -c "pid=\"$pid\"" /tmp/pwh_openmetrics.txt > /tmp/pwh_fresh_pid_count.txt
\set fresh_pid_count `tr -d '[:space:]' < /tmp/pwh_fresh_pid_count.txt`
SELECT :'fresh_pid_count'::int > 0 AS cache_uses_sample_interval;

\! grep -c "^# HELP [a-zA-Z_:][a-zA-Z0-9_:]* " /tmp/pwh_openmetrics.txt > /tmp/pwh_help_count.txt
\! grep -c "^# TYPE [a-zA-Z_:][a-zA-Z0-9_:]* " /tmp/pwh_openmetrics.txt > /tmp/pwh_type_count.txt

\set help_count `tr -d '[:space:]' < /tmp/pwh_help_count.txt`
\set type_count `tr -d '[:space:]' < /tmp/pwh_type_count.txt`
SELECT
  :'help_count'::int > 0 AS has_help_lines,
  :'type_count'::int > 0 AS has_type_lines,
  :'help_count'::int = :'type_count'::int AS help_type_counts_match;

\set content_type_valid `grep -qi '^content-type: application/openmetrics-text; version=1.0.0; charset=utf-8' /tmp/pwh_openmetrics_headers.txt && printf true || printf false`
\set end_marker_valid `tail -n 1 /tmp/pwh_openmetrics.txt | grep -qx '# EOF' && printf true || printf false`
SELECT
  :'content_type_valid'::bool AS content_type_valid,
  :'end_marker_valid'::bool AS has_end_marker;

\! grep "^# TYPE " /tmp/pwh_openmetrics.txt | cut -d ' ' -f 4 | sort | uniq > /tmp/pwh_types.txt
\! grep -c "gauge\|counter\|histogram\|summary\|untyped" /tmp/pwh_types.txt > /tmp/pwh_valid_types.txt
\set valid_type_count `tr -d '[:space:]' < /tmp/pwh_valid_types.txt`
SELECT :'valid_type_count'::int > 0 AS has_valid_metric_types;

\! grep -v "^#" /tmp/pwh_openmetrics.txt | grep -v "^$" | head -1 > /tmp/pwh_sample_metric.txt
\set metric_name_valid `grep -Eq '^[a-zA-Z_:][a-zA-Z0-9_:]*\{' /tmp/pwh_sample_metric.txt && printf true || printf false`
\set labels_valid `grep -Eq '[a-zA-Z_][a-zA-Z0-9_]*="[^"]*"' /tmp/pwh_sample_metric.txt && printf true || printf false`
\set numeric_value_valid `grep -Eq '\} [0-9eE.+-]+' /tmp/pwh_sample_metric.txt && printf true || printf false`
SELECT
  :'metric_name_valid'::bool AS metric_name_valid,
  :'labels_valid'::bool AS labels_properly_quoted,
  :'numeric_value_valid'::bool AS has_numeric_value;

\! grep -c "^[a-zA-Z_:][a-zA-Z0-9_:]*{.*}" /tmp/pwh_openmetrics.txt > /tmp/pwh_metric_lines.txt
\set metric_line_count `tr -d '[:space:]' < /tmp/pwh_metric_lines.txt`
SELECT :'metric_line_count'::int > 0 AS has_metric_value_lines;

SELECT pg_advisory_unlock(12351);
SELECT pg_sleep(2);

\! perl -MIO::Socket::INET -e '$s=IO::Socket::INET->new(PeerAddr=>"127.0.0.1",PeerPort=>9187,Proto=>"tcp") or exit 1; print $s "G"; sleep 2' &
SELECT pg_sleep(0.1);
\set slow_client_code `curl --silent --max-time 1.5 -o /dev/null -w "%{http_code}" http://localhost:9187/metrics`
SELECT :'slow_client_code' = '200' AS slow_client_does_not_block_server;

ALTER SYSTEM SET what_is_happening.metrics_max_response_bytes = 1024;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
\set bounded_code `curl --silent --max-time 5 -o /dev/null -w "%{http_code}" http://localhost:9187/metrics`
SELECT :'bounded_code' = '500' AS oversized_response_is_rejected;

ALTER SYSTEM RESET what_is_happening.metrics_max_response_bytes;
ALTER SYSTEM RESET what_is_happening.signal_timeout_ms;
ALTER SYSTEM RESET what_is_happening.sample_interval_ms;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
\set restored_code `curl --silent --max-time 5 -o /dev/null -w "%{http_code}" http://localhost:9187/metrics`
SELECT :'restored_code' = '200' AS response_limit_is_reloadable;

\! rm -f /tmp/pwh_*.txt
