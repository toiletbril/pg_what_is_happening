#!/bin/bash

#
# See Shfile.sh.
#

set -eu

postgres_started=false
cleanup()
{
  if test "$postgres_started" = true; then
    pg_ctl -D "$PG_DATA_DIR" stop >/dev/null 2>&1 || true
    postgres_started=false
  fi
}
trap cleanup EXIT HUP INT TERM

cd '/pg_what_is_happening'
. "scripts/common.sh"
init_env

build_postgresql_if_not_built

echo "Installing extension from ./pg_what_is_happening.so..."
make install -j"$(parallel_jobs)"

ulimit -c unlimited 2>/dev/null || true

init_postgresql_data_dir
edit_postgresql_conf
start_postgresql
postgres_started=true

if ! make -C /pg_what_is_happening installcheck; then
  echo "ERROR: Tests failed"
  echo ""
  tail -n 500 "$PG_LOG_FILE" | cat -n
  echo ""
  test -f "$PG_PWH_SOURCE/test/regression.diffs" && \
    cat -n "$PG_PWH_SOURCE/test/regression.diffs"
  exit 1
fi
