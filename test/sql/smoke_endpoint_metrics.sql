-- Test OpenMetrics endpoint with async complex query running in background.
-- This test launches a slow query and queries /metrics while it's running.

SELECT pg_advisory_lock(12347);

\! psql -d contrib_regression -c "SELECT pg_advisory_lock(12347), COUNT(*) FROM orders o JOIN users u ON o.user_id = u.user_id JOIN products p ON o.user_id = p.product_id JOIN reviews r ON p.product_id = r.product_id; SELECT pg_advisory_unlock(12347);" > /dev/null 2>&1 &

SELECT pg_sleep(0.2);

-- Query the /metrics endpoint while the query is running.
-- We should see OpenMetrics output with node-level metrics.
\! curl -s http://localhost:9187/metrics > /data/pwh_metrics_output.txt

-- Verify OpenMetrics format - check for HELP and TYPE declarations.
\set help_count `grep -c "^# HELP pg_what_is_happening_active_query_node" /data/pwh_metrics_output.txt`
\set type_count `grep -c "^# TYPE pg_what_is_happening_active_query_node" /data/pwh_metrics_output.txt`

SELECT
  :help_count > 0 AS has_help_declarations,
  :type_count > 0 AS has_type_declarations,
  :type_count = :help_count AS as_many_help_as_types;

-- Verify actual metric lines are present with labels and values.
\set metric_count `grep -c "^pg_what_is_happening_active_query_node_.*{query_id=" /data/pwh_metrics_output.txt`
SELECT
  :metric_count > 96 AS has_sufficient_metrics;

-- Check HTTP status code is 200.
\set http_code `curl -s -o /dev/null -w "%{http_code}" http://localhost:9187/metrics`
SELECT
  :'http_code' = '200' AS http_status_ok;

SELECT pg_advisory_unlock(12347);
SELECT pg_sleep(0.5);

-- Cleanup temp files.
\! rm -f /data/pwh_*.txt
