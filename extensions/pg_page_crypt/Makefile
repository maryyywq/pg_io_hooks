# extensions/pg_page_crypt/Makefile

MODULES = pg_page_crypt
PROGRAM = pg_page_crypt_convert
OBJS = pg_page_crypt_convert.o

PGFILEDESC = "pg_page_crypt - transparent encryption of relation files through smgr hooks"

SHLIB_LINK = -lcrypto
PG_LIBS = -lcrypto

TAP_TESTS = 1

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
