-- Test graceful handling when all backend slots are exhausted.

-- Get the max_tracked_queries GUC value.
SELECT current_setting('what_is_happening.max_tracked_queries')::int AS max_tracked_queries
\gset

-- Launch more queries than the limit to test overflow handling.
\! for i in $(seq 1 130); do psql -d contrib_regression -c "SELECT pg_sleep(5), COUNT(*) FROM orders LIMIT 1;" > /dev/null 2>&1 & done

SELECT pg_sleep(2);

-- Count how many queries are actually being tracked.
-- Should be exactly max_tracked_queries.
SELECT
  COUNT(DISTINCT backend_pid) = :max_tracked_queries AS tracked_equals_limit
FROM what_is_happening.v1_status
WHERE query_text LIKE '%pg_sleep(5)%';

-- Verify system handles overflow gracefully without crashes.
SELECT
  COUNT(*) FILTER (WHERE node_id < 0) = 0 AS valid_node_ids,
  COUNT(*) FILTER (WHERE backend_pid <= 0) = 0 AS valid_backend_pids
FROM what_is_happening.v1_status
WHERE query_text LIKE '%pg_sleep(5)%';

-- Wait for all background queries to finish.
SELECT pg_sleep(8);
