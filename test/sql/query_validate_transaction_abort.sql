-- Test cleanup when transaction aborts.
-- This test verifies that the extension properly cleans up metrics after abort.

-- Launch a query in a transaction that will abort.
\! psql -d contrib_regression -c "BEGIN; SELECT pg_sleep(0.2), COUNT(*) FROM orders o JOIN users u ON o.user_id = u.user_id; SELECT 1 / 0;" > /dev/null 2>&1 &

SELECT pg_sleep(0.05);

-- Check that query is being tracked while running.
SELECT
  COUNT(*) > 0 AS query_tracked_during_execution
FROM what_is_happening.v1_status
WHERE backend_pid <> pg_backend_pid() AND query_text LIKE '%COUNT(*)%' AND query_text LIKE '%orders%' AND query_text LIKE '%users%';

-- Wait for transaction to abort.
SELECT pg_sleep(0.3);

-- Verify cleanup after abort - query should be gone from v1_status.
SELECT
  COUNT(*) = 0 AS query_cleaned_up_after_abort
FROM what_is_happening.v1_status
WHERE backend_pid <> pg_backend_pid() AND query_text LIKE '%COUNT(*)%' AND query_text LIKE '%orders%' AND query_text LIKE '%users%';

-- End.
