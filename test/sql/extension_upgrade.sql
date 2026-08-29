CREATE EXTENSION pg_what_is_happening VERSION '1.0';
ALTER EXTENSION pg_what_is_happening UPDATE TO '1.2';
SELECT extversion = '1.2' AS upgraded_from_1_0
FROM pg_extension
WHERE extname = 'pg_what_is_happening';
DROP EXTENSION pg_what_is_happening;

CREATE EXTENSION pg_what_is_happening VERSION '1.1';
ALTER EXTENSION pg_what_is_happening UPDATE TO '1.2';
SELECT extversion = '1.2' AS upgraded_from_1_1
FROM pg_extension
WHERE extname = 'pg_what_is_happening';
DROP EXTENSION pg_what_is_happening;

CREATE EXTENSION pg_what_is_happening;
SELECT extversion = '1.2' AS fresh_version_is_1_2
FROM pg_extension
WHERE extname = 'pg_what_is_happening';
DROP EXTENSION pg_what_is_happening;

-- End.
