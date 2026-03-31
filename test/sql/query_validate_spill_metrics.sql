\! psql -d contrib_regression -c "SET work_mem = '64kB'; SELECT pg_sleep(0.03), o.order_id, SUM(p.amount) FROM orders o JOIN payments p ON o.order_id = p.order_id GROUP BY o.order_id ORDER BY SUM(p.amount) DESC;" > /dev/null 2>&1 &

SELECT pg_sleep(0.05);

SELECT
  COUNT(*) FILTER (WHERE node_tag = 'Hash' AND (spill_file_reads > 0 OR spill_file_writes > 0)) > 0 AS hash_spilled_to_disk,
  COUNT(*) FILTER (WHERE node_tag = 'Sort' AND (spill_file_reads > 0 OR spill_file_writes > 0)) >= 0 AS sort_may_spill
FROM what_is_happening.v1_status
WHERE query_text LIKE '%pg_sleep(0.03)%';
