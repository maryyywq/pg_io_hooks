# extensions/pg_io_audit/Makefile

MODULES = pg_io_audit
EXTENSION = pg_io_audit
DATA = pg_io_audit--1.0.sql
PGFILEDESC = "pg_io_audit - audit of storage I/O operations through smgr hooks"

TAP_TESTS = 1

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
