/* contrib/dbobject/dbobject--1.0.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION dbobject UPDATE TO '1.0'" to load this file. \quit

CREATE FUNCTION dbobject_indexdef(
    indexrelid oid
)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_dbobject'
LANGUAGE C STRICT;

