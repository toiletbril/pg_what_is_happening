\! psql -d contrib_regression -c "SET enable_hashjoin = off; SET enable_mergejoin = off; SELECT w.warehouse_name, o.order_id FROM warehouses w JOIN shipping s ON w.warehouse_id = s.warehouse_id JOIN orders o ON s.tracking_number = o.tracking_number;" > /dev/null 2>&1 &

SELECT pg_sleep(0.1);

SELECT
  MAX(loops_executed) > 1 AS any_node_looped
FROM what_is_happening.v1_status
WHERE query_text LIKE '%warehouses%' AND query_text LIKE '%orders%';
