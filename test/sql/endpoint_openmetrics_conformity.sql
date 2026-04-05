SELECT pg_advisory_lock(12351);

\! psql -d contrib_regression -c "SELECT pg_advisory_lock(12351), COUNT(*) FROM orders o JOIN users u ON o.user_id = u.user_id LIMIT 10; SELECT pg_advisory_unlock(12351);" > /dev/null 2>&1 &

SELECT pg_sleep(0.2);

\! curl -s http://localhost:9187/metrics > /data/pwh_openmetrics.txt

\! grep -c "^# HELP [a-zA-Z_:][a-zA-Z0-9_:]* " /data/pwh_openmetrics.txt > /data/pwh_help_count.txt
\! grep -c "^# TYPE [a-zA-Z_:][a-zA-Z0-9_:]* " /data/pwh_openmetrics.txt > /data/pwh_type_count.txt

SELECT
  (SELECT pg_read_file('pwh_help_count.txt')::text::int) > 0 AS has_help_lines,
  (SELECT pg_read_file('pwh_type_count.txt')::text::int) > 0 AS has_type_lines,
  (SELECT pg_read_file('pwh_help_count.txt')::text::int) = (SELECT pg_read_file('pwh_type_count.txt')::text::int) AS help_type_counts_match;

\! grep "^# TYPE " /data/pwh_openmetrics.txt | awk '{print $4}' | sort | uniq > /data/pwh_types.txt
\! grep -c "gauge\|counter\|histogram\|summary\|untyped" /data/pwh_types.txt > /data/pwh_valid_types.txt
SELECT (SELECT pg_read_file('pwh_valid_types.txt')::text::int) > 0 AS has_valid_metric_types;

\! grep -v "^#" /data/pwh_openmetrics.txt | grep -v "^$" | head -1 > /data/pwh_sample_metric.txt
SELECT
  pg_read_file('pwh_sample_metric.txt')::text ~ '^[a-zA-Z_:][a-zA-Z0-9_:]*\{' AS metric_name_valid,
  pg_read_file('pwh_sample_metric.txt')::text ~ '[a-zA-Z_][a-zA-Z0-9_]*="[^"]*"' AS labels_properly_quoted,
  pg_read_file('pwh_sample_metric.txt')::text ~ '\} [0-9eE.+-]+' AS has_numeric_value;

\! grep -c "^[a-zA-Z_:][a-zA-Z0-9_:]*{.*}" /data/pwh_openmetrics.txt > /data/pwh_metric_lines.txt
SELECT (SELECT pg_read_file('pwh_metric_lines.txt')::text::int) > 0 AS has_metric_value_lines;

SELECT pg_advisory_unlock(12351);
SELECT pg_sleep(0.5);

\! rm -f /data/pwh_*.txt
