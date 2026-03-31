\! psql -d contrib_regression -c "SET enable_hashjoin = off; SET enable_mergejoin = off; SELECT pg_sleep(0.1), w.warehouse_name, COUNT(*) FROM warehouses w JOIN shipping s ON w.warehouse_id = s.warehouse_id GROUP BY w.warehouse_name LIMIT 5;" > /dev/null 2>&1 &

SELECT pg_sleep(0.05);

SELECT
  COUNT(*) FILTER (WHERE node_tag = 'NestedLoop') > 0 AS has_nested_loop,
  MAX(loops_executed) > 1 AS inner_side_looped
FROM what_is_happening.v1_status
WHERE query_text LIKE '%pg_sleep(0.1)%';
