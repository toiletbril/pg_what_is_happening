#!/bin/bash

#
# See Shfile.sh.
#

set -eu

cd '/pg_what_is_happening'
. "scripts/common.sh"
init_env

build_postgresql_if_not_built

echo "Installing extension from ./pg_what_is_happening.so..."
make install -j"$(nproc)"

# Allow core files.
ulimit -c unlimited

init_postgresql_data_dir
edit_postgresql_conf
start_postgresql

env | grep 'PG_'

exec bash -i
