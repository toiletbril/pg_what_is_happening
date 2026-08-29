CREATE EXTENSION pg_what_is_happening VERSION '1.0';
SELECT has_schema_privilege('public', 'what_is_happening', 'USAGE') AS version_1_0_schema_grant;
ALTER EXTENSION pg_what_is_happening UPDATE TO '1.1';
ALTER EXTENSION pg_what_is_happening UPDATE TO '1.2';
SELECT extversion = '1.2' AS upgraded_from_1_0_through_1_1
FROM pg_extension
WHERE extname = 'pg_what_is_happening';
SELECT p.provolatile = 'v' AS volatile_function,
       p.proretset AS set_returning_function,
       array_length(p.proallargtypes, 1) = 20 AS output_contract,
       has_function_privilege('public', p.oid, 'EXECUTE') AS function_grant,
       has_table_privilege('public', 'what_is_happening.v1_status', 'SELECT') AS view_grant,
       has_schema_privilege('public', 'what_is_happening', 'USAGE') AS schema_grant
FROM pg_proc p
WHERE p.oid = 'what_is_happening.v1_status_f()'::regprocedure;
DROP EXTENSION pg_what_is_happening;

CREATE EXTENSION pg_what_is_happening VERSION '1.0';
ALTER EXTENSION pg_what_is_happening UPDATE TO '1.2';
SELECT extversion = '1.2' AS upgraded_directly_from_1_0
FROM pg_extension
WHERE extname = 'pg_what_is_happening';
DROP EXTENSION pg_what_is_happening;

CREATE EXTENSION pg_what_is_happening VERSION '1.1';
SELECT has_schema_privilege('public', 'what_is_happening', 'USAGE') AS version_1_1_schema_grant;
ALTER EXTENSION pg_what_is_happening UPDATE TO '1.2';
SELECT extversion = '1.2' AS upgraded_from_1_1
FROM pg_extension
WHERE extname = 'pg_what_is_happening';
SELECT p.provolatile = 'v' AS volatile_function,
       p.proretset AS set_returning_function,
       array_length(p.proallargtypes, 1) = 20 AS output_contract,
       has_function_privilege('public', p.oid, 'EXECUTE') AS function_grant,
       has_table_privilege('public', 'what_is_happening.v1_status', 'SELECT') AS view_grant,
       has_schema_privilege('public', 'what_is_happening', 'USAGE') AS schema_grant
FROM pg_proc p
WHERE p.oid = 'what_is_happening.v1_status_f()'::regprocedure;
DROP EXTENSION pg_what_is_happening;

CREATE EXTENSION pg_what_is_happening;
SELECT extversion = '1.2' AS fresh_version_is_1_2
FROM pg_extension
WHERE extname = 'pg_what_is_happening';
SELECT p.provolatile = 'v' AS volatile_function,
       p.proretset AS set_returning_function,
       array_length(p.proallargtypes, 1) = 20 AS output_contract,
       has_function_privilege('public', p.oid, 'EXECUTE') AS function_grant,
       has_table_privilege('public', 'what_is_happening.v1_status', 'SELECT') AS view_grant,
       has_schema_privilege('public', 'what_is_happening', 'USAGE') AS schema_grant
FROM pg_proc p
WHERE p.oid = 'what_is_happening.v1_status_f()'::regprocedure;

BEGIN;
DROP EXTENSION pg_what_is_happening;
SELECT NOT EXISTS (
  SELECT 1 FROM pg_namespace WHERE nspname = 'what_is_happening'
) AS extension_objects_removed;
ROLLBACK;
SELECT extversion = '1.2' AS rollback_restored_extension
FROM pg_extension
WHERE extname = 'pg_what_is_happening';
DROP EXTENSION pg_what_is_happening;

-- End.
