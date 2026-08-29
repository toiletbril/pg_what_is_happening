-- Test graceful handling when all backend slots are exhausted.

SELECT pg_advisory_lock(12353);

-- Launch more queries than the limit to test overflow handling.
\! for i in 1 2 3 4 5 6 7 8 9 10 11 12; do PGAPPNAME="pwh_slot_$i" psql -X -v ON_ERROR_STOP=1 -d contrib_regression -c "SELECT pg_advisory_lock(12353), COUNT(*) FROM orders LIMIT 1; SELECT pg_advisory_unlock(12353);" > /dev/null 2>&1 & done

\! sleep 1
SELECT COUNT(*) = 12 AS all_helper_backends_started
FROM pg_stat_activity
WHERE application_name LIKE 'pwh_slot_%';

-- Count how many queries are actually being tracked.
SELECT
  COUNT(DISTINCT backend_pid) = 7 AS tracked_helpers_fill_available_slots
FROM what_is_happening.v1_status
WHERE backend_pid IN (
  SELECT pid FROM pg_stat_activity WHERE application_name LIKE 'pwh_slot_%'
);

SELECT COUNT(*) = 5 AS overflow_queries_remain_untracked
FROM pg_stat_activity a
WHERE a.application_name LIKE 'pwh_slot_%'
  AND NOT EXISTS (
    SELECT 1 FROM what_is_happening.v1_status s WHERE s.backend_pid = a.pid
  );

-- Verify system handles overflow gracefully without crashes.
SELECT
  COUNT(*) FILTER (WHERE node_id < 0) = 0 AS valid_node_ids,
  COUNT(*) FILTER (WHERE backend_pid <= 0) = 0 AS valid_backend_pids
FROM what_is_happening.v1_status
WHERE backend_pid IN (
  SELECT pid FROM pg_stat_activity WHERE application_name LIKE 'pwh_slot_%'
);

-- Wait for all background queries to finish.
SELECT pg_advisory_unlock(12353);
\! sleep 1
SELECT COUNT(*) = 0 AS helper_backends_finished
FROM pg_stat_activity
WHERE application_name LIKE 'pwh_slot_%';

-- End.
