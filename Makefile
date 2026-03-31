#
# This file is supposed to be used only from Docker container or a host with a
# running postgres instance on production-like environments.
#
# For development, please use Shfile.sh wrapper.
#

MODE ?= dbg

WITH_BGWORKER ?= yes
HTTP_BACKEND ?= mongoose

ifndef VERBOSE
MAKEFLAGS += -s
endif

ifneq ($(filter fmt tidy reset manual-install,$(MAKECMDGOALS)),)
# Utility targets don't need PGXS.
PG_CONFIG ?= true
PGXS := /dev/null
else
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
endif

MODULE_big = pg_what_is_happening
EXTENSION = pg_what_is_happening
DATA = pg_what_is_happening--1.0.sql pg_what_is_happening--1.0--1.1.sql
REGRESS_OPTS = --inputdir=test --outputdir=test --schedule=test/schedule
REGRESS = teardown
EXTRA_CLEAN = src/o/

include src/Makefile

ifeq ($(filter fmt tidy reset manual-install,$(MAKECMDGOALS)),)
include $(PGXS)
endif

CLANG_FORMAT ?= clang-format

fmt:
	echo "    " CLANG_FORMAT -i src/**/*.c src/**/*.h src/*.c src/*.h
	$(CLANG_FORMAT) -i src/*.c src/*.h src/*/*.c src/*/*.h

CLANG_TIDY ?= clang-tidy

tidy:
	echo "    " CLANG_TIDY src/*/*.c src/*/*.h src/*.c src/*.h)
	$(CLANG_TIDY) src/*.c src/*.h src/*/*.c src/*/*.h --extra-arg=-std=c17

reset:
	echo "    " RM src/o/$(MODE) pg_what_is_happening.so
	rm -rf src/o/$(MODE) pg_what_is_happening.so

dev-reset:
	$(MAKE) reset
	$(MAKE) install -j$(shell nproc)
	pg_ctl -D /data -l /tmp/postgres.log restart

.PHONY: fmt tidy reset relaunch dirs
