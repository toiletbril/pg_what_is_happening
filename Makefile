#
# This file is supposed to be used only from Docker container or a host with a
# running postgres instance on production-like environments.
#
# For development, please use Shfile.sh wrapper.
#

.DEFAULT_GOAL := all

MODE ?= rel

WITH_BGWORKER ?= yes
HTTP_BACKEND ?= mongoose

ifndef VERBOSE
MAKEFLAGS += -s
endif

ifneq ($(filter fmt reset validate-dashboard,$(MAKECMDGOALS)),)
# Utility targets don't need PGXS.
PG_CONFIG ?= true
PGXS := /dev/null
else
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
endif

MODULE_big = pg_what_is_happening
EXTENSION = pg_what_is_happening
DATA = pg_what_is_happening--1.0.sql pg_what_is_happening--1.0--1.1.sql pg_what_is_happening--1.0--1.2.sql pg_what_is_happening--1.1.sql pg_what_is_happening--1.1--1.2.sql pg_what_is_happening--1.2.sql
REGRESS_OPTS = --inputdir=test --outputdir=test --schedule=test/schedule
REGRESS = teardown
EXTRA_CLEAN = src/o/ pg_what_is_happening.dylib

include src/Makefile

ifeq ($(filter fmt reset validate-dashboard,$(MAKECMDGOALS)),)
include $(PGXS)
endif

ACTIVE_BUILD_CONFIG := src/o/.active-build-config

$(ACTIVE_BUILD_CONFIG): FORCE | dirs
	printf '%s\n' '$(BUILD_KEY)' > $@.tmp
	if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

pg_what_is_happening$(DLSUFFIX): $(ACTIVE_BUILD_CONFIG)

CLANG_FORMAT ?= clang-format

fmt:
	echo "    " CLANG_FORMAT -i src/**/*.c src/**/*.h src/*.c src/*.h
	$(CLANG_FORMAT) -i src/*.c src/*.h src/*/*.c src/*/*.h

CLANG_TIDY ?= clang-tidy

tidy:
	echo "    " CLANG_TIDY $(SOURCES)
	$(CLANG_TIDY) $(filter src/%,$(SOURCES)) -- $(CPPFLAGS) $(CFLAGS2)

reset:
	echo "    " RM src/o pg_what_is_happening.so pg_what_is_happening.dylib
	rm -rf src/o pg_what_is_happening.so pg_what_is_happening.dylib

validate-dashboard:
	sh scripts/validate-dashboard.sh

dev-reset:
	$(MAKE) reset
	$(MAKE) install -j$(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
	pg_ctl -D /data -l /tmp/postgresql.log restart

.PHONY: fmt tidy reset validate-dashboard dev-reset dirs
