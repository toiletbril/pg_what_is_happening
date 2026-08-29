SELECT pg_advisory_lock(12347);
\! psql -d contrib_regression -c "SET work_mem = '64kB'; SELECT pg_advisory_lock(12347), COUNT(*) FROM (SELECT i, md5(i::text), md5(md5(i::text)) FROM generate_series(1, 500000) i ORDER BY md5(i::text) DESC OFFSET 0) spilled; SELECT pg_advisory_unlock(12347);" > /dev/null 2>&1 &

SELECT pg_sleep(1);

SELECT
  MAX(spill_file_reads + spill_file_writes) > 0 AS has_spill_metrics
FROM what_is_happening.v1_status
WHERE query_text LIKE '%md5(md5%';

SELECT pg_advisory_unlock(12347);
SELECT pg_sleep(0.2);

-- End.
