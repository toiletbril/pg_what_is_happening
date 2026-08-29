#!/bin/bash

#
# See Shfile.sh.
#

set -eu

cd '/pg_what_is_happening'
. "scripts/common.sh"
init_env
build_postgresql_if_not_built

echo "Building pg_what_is_happening from source..."
make -j"$(parallel_jobs)" all

ls -lah pg_what_is_happening.so
