#!/bin/bash

#
# Common utilities for bash scripts. Also See Shfile.sh.
#

export PG_SOURCE="/postgres"
export PG_PWH_SOURCE='/pg_what_is_happening'
export PG_DATA_DIR='/data'
export PG_BIN_DIR='/postgres-bin'
export PG_LOG_FILE='/tmp/postgresql.log'

export PATH=/postgres-bin/bin:$PATH

build_postgresql_if_not_built() {
  if ! test -f "$PG_SOURCE/config.status"; then
    echo "Configuring PostgreSQL..."
    cd "$PG_SOURCE" || return
    PG_CFLAGS="-std=gnu11 -g3 -O0 -Wno-error=incompatible-pointer-types"
    ./configure --prefix="$PG_SOURCE" --enable-debug --enable-cassert CFLAGS="$PG_CFLAGS" >/dev/null
    cd - || return
  fi
  echo "Building PostgreSQL from source..."
  make -C /postgres -s -j"$(nproc)" install
}

init_postgresql_data_dir()
{
  echo "Initializing database..."
  initdb -D "$PG_DATA_DIR"
}

edit_postgresql_conf()
{
  echo "Editing postgresql.conf..."
  cat >> "$1" <<EOF
shared_preload_libraries = 'pg_what_is_happening'
log_min_messages = debug1
max_connections = 50
what_is_happening.max_tracked_queries = 16
what_is_happening.max_nodes_per_query = 64
what_is_happening.max_query_text_length = 512
shared_buffers = 256MB
EOF
}

start_postgresql()
{
  echo "Starting PostgreSQL..."
  if ! pg_ctl -D "$PG_DATA_DIR" -l "$PG_LOG_FILE" start; then
    echo "ERROR: PostgreSQL failed to start"
    cat -n "$PG_LOG_FILE"
    exit 1
  fi
}
