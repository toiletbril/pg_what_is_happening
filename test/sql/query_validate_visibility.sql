CREATE ROLE pwh_owner;
CREATE ROLE pwh_reader;

\! psql -d contrib_regression -c "SET ROLE pwh_owner; SELECT pg_sleep(1);" > /dev/null 2>&1 &
SELECT pg_sleep(0.1);

SET ROLE pwh_reader;
SELECT bool_and(query_text = '<insufficient privilege>') AS other_role_is_redacted
FROM what_is_happening.v1_status
WHERE backend_pid <> pg_backend_pid();
RESET ROLE;

SET ROLE pwh_owner;
SELECT bool_or(query_text LIKE '%pg_sleep(1)%') AS owner_can_read
FROM what_is_happening.v1_status
WHERE backend_pid <> pg_backend_pid();
RESET ROLE;

SELECT pg_sleep(1);
DROP ROLE pwh_reader;
DROP ROLE pwh_owner;

-- End.
