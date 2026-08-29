-- Test cleanup when transaction aborts.
-- This test verifies that the extension properly cleans up metrics after abort.

\! PGAPPNAME=pwh_caught_abort psql -X -d contrib_regression -f test/sql/query_validate_transaction_abort_helper.sql > /dev/null 2>&1 &

SELECT pg_sleep(0.05);

-- Check that query is being tracked while running.
SELECT
  COUNT(*) > 0 AS query_tracked_during_execution
FROM what_is_happening.v1_status
WHERE backend_pid IN (
  SELECT pid FROM pg_stat_activity WHERE application_name = 'pwh_caught_abort'
) AND query_text LIKE '%COUNT(*)%orders%users%';

-- Wait for transaction to abort.
SELECT pg_sleep(0.4);

-- Verify cleanup after abort - query should be gone from v1_status.
SELECT
  COUNT(*) = 0 AS query_cleaned_up_after_abort
FROM what_is_happening.v1_status
WHERE backend_pid IN (
  SELECT pid FROM pg_stat_activity WHERE application_name = 'pwh_caught_abort'
) AND query_text LIKE '%COUNT(*)%orders%users%';

SELECT COUNT(*) = 1 AS backend_survived_caught_abort
FROM pg_stat_activity
WHERE application_name = 'pwh_caught_abort';

SELECT pg_sleep(0.8);

-- End.
