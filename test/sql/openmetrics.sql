-- Test OpenMetrics endpoint.
-- This test verifies the /metrics endpoint returns Prometheus-formatted metrics.

SET client_min_messages TO warning;

-- Setup test table.
DROP TABLE IF EXISTS metrics_test CASCADE;
CREATE TABLE metrics_test AS
SELECT i, i * 2 as val
FROM generate_series(1, 10000) i;

ANALYZE metrics_test;

-- Run a query that will generate metrics.
SELECT COUNT(*) FROM metrics_test WHERE val > 5000;

-- Give the extension time to process.
SELECT pg_sleep(0.1);

-- Query the /metrics endpoint using curl.
-- This assumes the default listen address of 127.0.0.1:9187.
\! curl -s http://127.0.0.1:9187/metrics | head -30

-- Verify endpoint is responding.
\! curl -s -o /dev/null -w "HTTP Status: %{http_code}\n" http://127.0.0.1:9187/metrics

-- Check for expected metric names in output.
\! curl -s http://127.0.0.1:9187/metrics | grep -c "pg_what_is_happening_active_query_node"

-- Check for specific metrics we expect.
\! curl -s http://127.0.0.1:9187/metrics | grep -E "^# (HELP|TYPE) pg_what_is_happening" | head -10

-- Cleanup.
DROP TABLE metrics_test;
