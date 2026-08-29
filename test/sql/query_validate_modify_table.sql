-- Verify that modification plans include their executable child nodes.
\! psql -d contrib_regression -c "UPDATE products SET price = price WHERE product_id <= 50 RETURNING pg_sleep(0.05);" > /dev/null 2>&1 &

SELECT pg_sleep(0.2);

SELECT
  COUNT(*) FILTER (WHERE node_tag = 'ModifyTable') > 0 AS has_modify_table,
  COUNT(*) FILTER (WHERE node_tag IN ('IndexScan', 'SeqScan')) > 0 AS has_scan_child,
  COUNT(*) = COUNT(DISTINCT backend_pid || '-' || query_id || '-' || node_id) AS no_duplicate_nodes,
  COUNT(*) FILTER (WHERE parent_node_id >= 0 AND NOT EXISTS (
    SELECT 1
    FROM what_is_happening.v1_status parent
    WHERE parent.backend_pid = child.backend_pid
      AND parent.query_id = child.query_id
      AND parent.node_id = child.parent_node_id
  )) = 0 AS valid_parent_nodes
FROM what_is_happening.v1_status child
WHERE backend_pid <> pg_backend_pid()
  AND query_text LIKE 'UPDATE products%RETURNING pg_sleep%';

SELECT pg_sleep(3);

-- End.
