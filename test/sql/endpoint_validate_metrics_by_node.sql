SELECT pg_advisory_lock(12348);

\! PGAPPNAME=pwh_endpoint_nodes psql -X -v ON_ERROR_STOP=1 -d contrib_regression -c "SELECT pg_advisory_lock(12348), o.order_id, u.username, p.product_name, SUM(o.quantity * p.price) as total FROM orders o JOIN users u ON o.user_id = u.user_id JOIN products p ON o.product_id = p.product_id GROUP BY o.order_id, u.username, p.product_name ORDER BY total DESC; SELECT pg_advisory_unlock(12348);" > /dev/null 2>&1 &

SELECT pg_sleep(0.4);

\! rm -f /tmp/pwh_target_pid.txt
COPY (
  SELECT pid FROM pg_stat_activity
  WHERE application_name = 'pwh_endpoint_nodes'
) TO '/tmp/pwh_target_pid.txt';

\! curl --fail --silent --show-error --max-time 5 http://localhost:9187/metrics > /tmp/pwh_endpoint_metrics.txt

\! grep "^pg_what_is_happening_active_query_node_time_percent{" /tmp/pwh_endpoint_metrics.txt | grep "pid=\"$(tr -d '[:space:]' < /tmp/pwh_target_pid.txt)\"" | awk '{sum += $NF} END {print sum + 0}' > /tmp/pwh_time_sum.txt
SELECT
  (SELECT pg_read_file('/tmp/pwh_time_sum.txt')::text::float8) BETWEEN 95 AND 105 AS time_percent_sums_to_100_per_query;

\! grep "^pg_what_is_happening_active_query_node" /tmp/pwh_endpoint_metrics.txt | grep "pid=\"$(tr -d '[:space:]' < /tmp/pwh_target_pid.txt)\"" | grep "node_tag=\"Hash" | wc -l > /tmp/pwh_hash_count.txt
SELECT (SELECT pg_read_file('/tmp/pwh_hash_count.txt')::text::int) > 0 AS has_hash_node_metrics;

\! grep "^pg_what_is_happening_active_query_node" /tmp/pwh_endpoint_metrics.txt | grep "pid=\"$(tr -d '[:space:]' < /tmp/pwh_target_pid.txt)\"" | grep "node_tag=\"Sort" | wc -l > /tmp/pwh_sort_count.txt
SELECT (SELECT pg_read_file('/tmp/pwh_sort_count.txt')::text::int) > 0 AS has_sort_node_metrics;

SELECT pg_advisory_unlock(12348);
SELECT pg_sleep(0.5);

\! rm -f /tmp/pwh_*.txt
