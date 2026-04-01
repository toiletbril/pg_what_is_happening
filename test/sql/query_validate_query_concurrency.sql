\! psql -d contrib_regression -c "SELECT o.order_id, u.username FROM orders o JOIN users u ON o.user_id = u.user_id ORDER BY o.order_id;" > /dev/null 2>&1 &
\! psql -d contrib_regression -c "SELECT p.product_id, r.rating FROM products p JOIN reviews r ON p.product_id = r.product_id ORDER BY p.product_id;" > /dev/null 2>&1 &

SELECT pg_sleep(0.2);

SELECT
  COUNT(DISTINCT backend_pid) >= 2 AS has_multiple_pids,
  COUNT(DISTINCT query_id) >= 2 AS has_multiple_query_ids
FROM what_is_happening.v1_status
WHERE query_text LIKE '%JOIN%' AND (query_text LIKE '%orders%' OR query_text LIKE '%products%');

SELECT
  COUNT(*) = COUNT(DISTINCT backend_pid || '-' || query_id || '-' || node_id) AS no_duplicate_nodes
FROM what_is_happening.v1_status
WHERE query_text LIKE '%JOIN%' AND (query_text LIKE '%orders%' OR query_text LIKE '%products%');

WITH orders_query AS (
  SELECT DISTINCT backend_pid FROM what_is_happening.v1_status WHERE query_text LIKE '%orders%' AND query_text LIKE '%JOIN%'
), products_query AS (
  SELECT DISTINCT backend_pid FROM what_is_happening.v1_status WHERE query_text LIKE '%products%' AND query_text LIKE '%JOIN%'
)
SELECT
  COUNT(*) FILTER (WHERE backend_pid IN (SELECT backend_pid FROM orders_query) AND query_text LIKE '%products%') = 0 AS orders_backend_has_no_products_metrics,
  COUNT(*) FILTER (WHERE backend_pid IN (SELECT backend_pid FROM products_query) AND query_text LIKE '%orders%') = 0 AS products_backend_has_no_orders_metrics
FROM what_is_happening.v1_status
WHERE query_text LIKE '%JOIN%';
