# contrib/pg_dbobject/Makefile

MODULE_big = pg_dbobject
OBJS = pg_dbobject.o $(WIN32RES)
PGFILEDESC = "pg_dbobject - PostgreSQL json object description"

PG_CPPFLAGS = -I$(libpq_srcdir)
SHLIB_LINK_INTERNAL = $(libpq)

EXTENSION = pg_dbobject
DATA = pg_dbobject--1.0.sql

REGRESS = pg_dbobject

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
SHLIB_PREREQS = submake-libpq
subdir = contrib/pg_dbobject
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
