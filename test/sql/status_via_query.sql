-- Test status view with async complex query running in background.
-- This test launches a slow query in another connection and queries metrics.

-- Launch async query that will run for ~0.1 second.
\! psql -d contrib_regression -c "SELECT pg_sleep(0.1), COUNT(*) FROM orders o JOIN users u ON o.user_id = u.user_id LIMIT 10;" > /dev/null 2>&1 &

-- Give the background query time to start and begin execution.
SELECT pg_sleep(0.05);

-- Query the status view to capture metrics from the running query.
-- We should see node-level execution metrics for the complex query.
SELECT
  COUNT(*) > 0 AS has_metrics,
  COUNT(DISTINCT query_id) > 0 AS has_query_id,
  COUNT(DISTINCT node_tag) > 0 AS has_node_types,
  COUNT(*) FILTER (WHERE node_tag LIKE '%Hash%') > 0 AS has_hash_nodes
FROM what_is_happening.v1_status
WHERE query_text LIKE '%pg_sleep(0.1)%';
