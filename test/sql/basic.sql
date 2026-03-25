-- Basic functionality tests for pg_what_is_happening

-- Check that schema was created
SELECT nspname FROM pg_namespace WHERE nspname = 'what_is_happening';

-- Check that function exists
SELECT proname FROM pg_proc
WHERE proname = 'v1_status_f'
AND pronamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'what_is_happening');

-- Check that view exists
SELECT viewname FROM pg_views
WHERE schemaname = 'what_is_happening' AND viewname = 'v1_status';

-- Test basic query execution and metrics capture
SELECT 1 + 1;

-- Query the status (should have at least our own backend)
SELECT COUNT(*) > 0 AS has_results FROM what_is_happening.v1_status;

-- Check column structure
SELECT
    column_name,
    data_type
FROM information_schema.columns
WHERE table_schema = 'what_is_happening'
AND table_name = 'v1_status'
ORDER BY ordinal_position;
