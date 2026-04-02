-- Test status view with async complex query running in background.
-- This test launches a slow query in another connection and queries metrics.

\! psql -d contrib_regression -c "SELECT u.region, p.category, COUNT(DISTINCT o.order_id) as order_count, SUM(pay.amount) as total_revenue, AVG(r.rating) as avg_rating, ROW_NUMBER() OVER (PARTITION BY u.region ORDER BY COUNT(*) DESC) as region_rank FROM orders o JOIN users u ON o.user_id = u.user_id JOIN products p ON o.product_id = p.product_id JOIN payments pay ON o.order_id = pay.order_id LEFT JOIN reviews r ON p.product_id = r.product_id AND o.user_id = r.user_id JOIN shipping s ON o.order_id = s.order_id JOIN warehouses w ON s.warehouse_id = w.warehouse_id WHERE u.region IN ('US', 'EU', 'APAC') AND p.price > 10 AND pay.amount > 0 GROUP BY u.region, p.category HAVING COUNT(*) > 10 ORDER BY total_revenue DESC;" > /dev/null 2>&1 &

SELECT pg_sleep(0.05);

-- Query the status view to capture metrics from the running query.
-- We should see node-level execution metrics for the complex query.
SELECT
  COUNT(*) > 0 AS has_metrics,
  COUNT(DISTINCT query_id) > 0 AS has_query_id,
  COUNT(DISTINCT node_tag) > 0 AS has_node_types,
  COUNT(*) FILTER (WHERE node_tag LIKE '%Hash%') > 0 AS has_hash_nodes
FROM what_is_happening.v1_status
WHERE query_text LIKE '%region_rank%';

-- Validate metric values are sane.
SELECT
  COUNT(*) FILTER (WHERE startup_time_us > total_time_us) = 0 AS time_consistency,
  COUNT(*) FILTER (WHERE tuples_returned < 0) = 0 AS valid_row_counts,
  COUNT(*) FILTER (WHERE loops_executed < 0) = 0 AS valid_loops,
  COUNT(*) FILTER (WHERE cache_hits < 0 OR cache_misses < 0) = 0 AS valid_buffer_stats,
  COUNT(*) FILTER (WHERE parent_node_id < -1) = 0 AS valid_parent_refs,
  COUNT(*) FILTER (WHERE node_id < 0) = 0 AS valid_node_ids
FROM what_is_happening.v1_status
WHERE query_text LIKE '%region_rank%';

-- Validate metrics are actually being captured with non-zero values.
SELECT
  COUNT(*) FILTER (WHERE tuples_returned > 0) > 0 AS has_nonzero_tuples,
  COUNT(*) FILTER (WHERE total_time_us > 0) > 0 AS has_nonzero_time,
  COUNT(*) FILTER (WHERE cache_hits + cache_misses > 0) > 0 AS has_buffer_activity
FROM what_is_happening.v1_status
WHERE query_text LIKE '%region_rank%';

-- Verify node hierarchy makes sense.
SELECT
  COUNT(*) FILTER (WHERE node_id = 0 AND parent_node_id = -1) > 0 AS has_root_node
FROM what_is_happening.v1_status
WHERE query_text LIKE '%region_rank%';

SELECT pg_sleep(2);
