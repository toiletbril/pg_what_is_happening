#!/bin/bash

set -xeu

sudo chown -R postgres:postgres /postgres /pg_what_is_happening

cd /pg_what_is_happening

if ! test -f /postgres/config.status; then
  echo "Configuring PostgreSQL..."
  cd /postgres
  ./configure --prefix=/postgres-bin --enable-debug --enable-cassert CFLAGS="-std=gnu11 -g3 -O0 -Wno-error=incompatible-pointer-types" >/dev/null
  cd -
fi

echo "Building PostgreSQL from source..."
make -C /postgres -s -j$(nproc) install
export PATH=/postgres-bin/bin:$PATH

echo "Installing extension from ./pg_what_is_happening.so..."
make -C /pg_what_is_happening install

ulimit -c unlimited

initdb -D /data

echo "Configuring postgresql.conf..."
cat >> /data/postgresql.conf <<EOF
shared_preload_libraries = 'pg_what_is_happening'
log_min_messages = debug1
max_connections = 50
pg_what_is_happening.max_tracked_queries = 16
pg_what_is_happening.max_nodes = 64
pg_what_is_happening.query_text_length = 512
EOF

if ! pg_ctl -D /data -l /tmp/postgres.log -w start; then
  echo "ERROR: PostgreSQL failed to start"
  cat /tmp/postgres.log
  exit 1
fi

if ! make -C /pg_what_is_happening installcheck; then
  echo "ERROR: Tests failed"
  echo ""
  echo "=== PostgreSQL log ==="
  cat /tmp/postgres.log
  pg_ctl -D /data stop
  echo ""
  if test -f /pg_what_is_happening/test/regression.diffs; then
    echo "=== Regression diffs ==="
    cat /pg_what_is_happening/test/regression.diffs
  fi
  exit 1
fi

pg_ctl -D /data stop
