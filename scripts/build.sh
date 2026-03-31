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

echo "Building pg_what_is_happening from source..."
make -C /pg_what_is_happening -j$(nproc) all

echo "Build completed successfully"

ls -lah pg_what_is_happening.so
