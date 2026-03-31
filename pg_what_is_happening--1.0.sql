/* pg_what_is_happening--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_what_is_happening" to load this file. \quit

-- Create schema to namespace extension objects
CREATE SCHEMA what_is_happening;

-- what_is_happening.v1_status_f()
CREATE FUNCTION what_is_happening.v1_status_f(
	OUT backend_pid int4,
	OUT query_id int8,
	OUT query_text text,
	OUT is_query_active boolean,
	OUT node_id int4,
	OUT parent_node_id int4,
	OUT node_tag text,
	OUT tuples_returned float8,
	OUT startup_time_us float8,
	OUT total_time_us float8,
	OUT loops_executed float8,
	OUT cache_hits int8,
	OUT cache_misses int8,
	OUT local_cache_hits int8,
	OUT local_cache_misses int8,
	OUT spill_file_reads int8,
	OUT spill_file_writes int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'v1_status_f'
LANGUAGE C STRICT VOLATILE;

GRANT EXECUTE ON FUNCTION what_is_happening.v1_status_f() TO PUBLIC;

-- what_is_happening.v1_status
CREATE VIEW what_is_happening.v1_status AS
SELECT * FROM what_is_happening.v1_status_f();

GRANT SELECT ON what_is_happening.v1_status TO PUBLIC;
