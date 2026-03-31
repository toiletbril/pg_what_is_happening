-- Test OpenMetrics endpoint with async complex query running in background.
-- This test launches a slow query and queries /metrics while it's running.

-- Launch async query that will run for ~0.1 second.
\! psql -d contrib_regression -c "SELECT pg_sleep(0.1), COUNT(*) FROM orders o JOIN users u ON o.user_id = u.user_id LIMIT 10;" > /dev/null 2>&1 &

-- Give the background query time to start and begin execution.
SELECT pg_sleep(0.05);

-- Query the /metrics endpoint while the query is running.
-- We should see OpenMetrics output with node-level metrics.
\! curl -s http://localhost:9187/metrics > /tmp/pwh_metrics_output.txt

-- Verify OpenMetrics format - check for HELP and TYPE declarations.
\! grep -c "^# HELP pg_what_is_happening_active_query_node" /tmp/pwh_metrics_output.txt > /tmp/pwh_help_count.txt
\! grep -c "^# TYPE pg_what_is_happening_active_query_node" /tmp/pwh_metrics_output.txt > /tmp/pwh_type_count.txt

SELECT
  (SELECT pg_read_file('/tmp/pwh_help_count.txt')::text::int) AS help_count,
  (SELECT pg_read_file('/tmp/pwh_help_count.txt')::text::int) > 0 AS has_help_declarations,
  (SELECT pg_read_file('/tmp/pwh_type_count.txt')::text::int) AS type_count,
  (SELECT pg_read_file('/tmp/pwh_type_count.txt')::text::int) > 0 AS has_type_declarations;

-- Verify actual metric lines are present (with labels and values).
\! grep -c "^pg_what_is_happening_active_query_node_.*{pid=" /tmp/pwh_metrics_output.txt > /tmp/pwh_metric_count.txt
SELECT
  (SELECT pg_read_file('/tmp/pwh_metric_count.txt')::text::int) > 200 AS has_sufficient_metrics;

-- Check HTTP status code is 200.
\! curl -s -o /dev/null -w "%{http_code}" http://localhost:9187/metrics > /tmp/pwh_status_code.txt
SELECT regexp_replace(pg_read_file('/tmp/pwh_status_code.txt')::text, E'[\\n\\r]+', '', 'g') = '200' AS http_status_ok;

-- Cleanup temp files.
\! rm -f /tmp/pwh_*.txt
