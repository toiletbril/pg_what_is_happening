\! psql -d contrib_regression -c "SELECT o.order_id, u.username, p.product_name, SUM(o.quantity * p.price) as total FROM orders o JOIN users u ON o.user_id = u.user_id JOIN products p ON o.product_id = p.product_id GROUP BY o.order_id, u.username, p.product_name ORDER BY total DESC;" > /dev/null 2>&1 &

SELECT pg_sleep(0.1);

\! curl -s http://localhost:9187/metrics > /data/pwh_endpoint_metrics.txt

\! grep "^pg_what_is_happening_active_query_node_time_percent{" /data/pwh_endpoint_metrics.txt | head -1 | sed 's/.*query_id="\([^"]*\)".*/\1/' > /data/pwh_query_id.txt
\! test -s /data/pwh_query_id.txt && grep "query_id=\"$(cat /data/pwh_query_id.txt)\"" /data/pwh_endpoint_metrics.txt | grep "time_percent{" | awk '{print $NF}' | awk '{s+=$1} END {print s}' > /data/pwh_time_sum.txt || echo "0" > /data/pwh_time_sum.txt
SELECT
  (SELECT pg_read_file('pwh_time_sum.txt')::text::float8) BETWEEN 95 AND 105 AS time_percent_sums_to_100_per_query;

\! grep "^pg_what_is_happening_active_query_node" /data/pwh_endpoint_metrics.txt | grep "node_tag=\"Hash" | wc -l > /data/pwh_hash_count.txt
SELECT (SELECT pg_read_file('pwh_hash_count.txt')::text::int) > 0 AS has_hash_node_metrics;

\! grep "^pg_what_is_happening_active_query_node" /data/pwh_endpoint_metrics.txt | grep "node_tag=\"Sort" | wc -l > /data/pwh_sort_count.txt
SELECT (SELECT pg_read_file('pwh_sort_count.txt')::text::int) > 0 AS has_sort_node_metrics;

\! rm -f /data/pwh_*.txt
