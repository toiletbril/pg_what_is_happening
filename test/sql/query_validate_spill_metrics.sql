\! psql -d contrib_regression -c "SET work_mem = '64kB'; SELECT i, md5(i::text), md5(md5(i::text)) FROM generate_series(1, 500000) i ORDER BY md5(i::text) DESC;" > /dev/null 2>&1 &

SELECT pg_sleep(1);

SELECT
  MAX(spill_file_reads + spill_file_writes) > 0 AS has_spill_metrics
FROM what_is_happening.v1_status
WHERE query_text LIKE '%md5(md5%';
