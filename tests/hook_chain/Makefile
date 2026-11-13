# tests/hook_chain/Makefile
#
# Test that several extensions can use the smgr hooks together.  Needs
# pg_io_audit and pg_page_crypt to be installed in the server under test.

TAP_TESTS = 1

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
