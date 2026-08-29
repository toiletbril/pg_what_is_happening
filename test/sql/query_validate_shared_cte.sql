-- Verify that shared CTE plan states appear only once in the metric topology.
\! psql -d contrib_regression -c "WITH shared_products AS (SELECT product_id FROM products WHERE product_id <= 100) SELECT pg_sleep(2), COUNT(*) FROM shared_products left_side JOIN shared_products right_side USING (product_id);" > /dev/null 2>&1 &

SELECT pg_sleep(0.2);

SELECT
  COUNT(*) FILTER (WHERE node_tag IN ('IndexOnlyScan', 'IndexScan', 'SeqScan')) = 1 AS one_physical_scan,
  COUNT(*) FILTER (WHERE node_tag = 'CteScan') = 2 AS two_cte_consumers,
  COUNT(*) = COUNT(DISTINCT backend_pid || '-' || query_id || '-' || node_id) AS no_duplicate_node_ids,
  COUNT(*) FILTER (WHERE parent_node_id >= 0 AND NOT EXISTS (
    SELECT 1
    FROM what_is_happening.v1_status parent
    WHERE parent.backend_pid = child.backend_pid
      AND parent.query_id = child.query_id
      AND parent.node_id = child.parent_node_id
  )) = 0 AS valid_parent_nodes
FROM what_is_happening.v1_status child
WHERE backend_pid <> pg_backend_pid()
  AND query_text LIKE 'WITH shared_products%';

SELECT pg_sleep(2.5);

-- End.
