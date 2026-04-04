#!/bin/bash

#
# See Shfile.sh.
#

set -xeu

build_postgresql_if_not_built

echo "Installing extension from ./pg_what_is_happening.so..."
make install "$(nproc)"

ulimit -c unlimited

init_postgresql_data_dir
edit_postgresql_conf
start_postgresql

if ! make -C /pg_what_is_happening installcheck; then
  echo "ERROR: Tests failed"
  echo ""
  cat -n "$PG_LOG_FILE"
  pg_ctl -D "$PG_DATA_DIR" stop
  echo ""
  test -f "$PG_PWH_SOURCE/test/regression.diffs" && \
    cat -n "$PG_PWH_SOURCE/test/regression.diffs"
  exit 1
fi

pg_ctl -D /data stop
