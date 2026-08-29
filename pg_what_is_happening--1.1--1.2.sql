\echo Use "ALTER EXTENSION pg_what_is_happening UPDATE TO '1.2'" to load this file. \quit

GRANT USAGE ON SCHEMA what_is_happening TO PUBLIC;
ALTER FUNCTION what_is_happening.v1_status_f() VOLATILE;
