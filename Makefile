OBJS = pg_chardetect.o flagcb.o
MODULE_big = pg_chardetect
DATA_built = pg_chardetect.sql
DOCS = README.pg_chardetect

PG_CPPFLAGS = -g -O0
SHLIB_LINK = -licuuc -licui18n -licudata

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
