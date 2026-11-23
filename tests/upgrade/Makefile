# tests/upgrade/Makefile
#
# Test pg_upgrade of a cluster encrypted with pg_page_crypt.  Needs two
# installations with pg_page_crypt installed in each: the one of PG_CONFIG
# (the new version) and the one named by OLD_INSTALL.  See
# scripts/run-upgrade-test.sh.

TAP_TESTS = 1

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
