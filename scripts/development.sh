#!/bin/bash

#
# See Shfile.sh.
#

set -xeu

. "$(realpath "$0")/common.sh"

build_postgresql_if_not_built

echo "Installing extension from ./pg_what_is_happening.so..."
make install -j"$(nproc)"

# Allow core files.
ulimit -c unlimited

init_postgresql_data_dir
edit_postgresql_conf '/data/postgresql.conf'
start_postgresql

env | grep 'PG_'

exec bash
