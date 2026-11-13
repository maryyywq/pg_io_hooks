/* pg_io_audit--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_io_audit" to load this file. \quit

CREATE FUNCTION pg_io_audit_get_stats(
    OUT op text,
    OUT fork text,
    OUT calls bigint,
    OUT blocks bigint)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION pg_io_audit_get_trace(
    OUT seq bigint,
    OUT ts timestamptz,
    OUT pid integer,
    OUT op text,
    OUT fork text,
    OUT tablespace oid,
    OUT database oid,
    OUT relfilenode oid,
    OUT block bigint,
    OUT nblocks integer)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION pg_io_audit_reset()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE VIEW pg_io_audit_stats AS
    SELECT * FROM pg_io_audit_get_stats();

CREATE VIEW pg_io_audit_trace AS
    SELECT * FROM pg_io_audit_get_trace();

-- The data describes I/O of the whole cluster, so restrict it.
REVOKE ALL ON FUNCTION pg_io_audit_get_stats() FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_io_audit_get_trace() FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_io_audit_reset() FROM PUBLIC;
REVOKE ALL ON pg_io_audit_stats FROM PUBLIC;
REVOKE ALL ON pg_io_audit_trace FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pg_io_audit_get_stats() TO pg_monitor;
GRANT EXECUTE ON FUNCTION pg_io_audit_get_trace() TO pg_monitor;
GRANT SELECT ON pg_io_audit_stats TO pg_monitor;
GRANT SELECT ON pg_io_audit_trace TO pg_monitor;
