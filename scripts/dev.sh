set -eu

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

echo "Initializing database..."
initdb -D /data

echo "Configuring postgresql.conf..."
cat >> /data/postgresql.conf <<EOF
max_connections = 50
shared_preload_libraries = 'pg_what_is_happening'
log_min_messages = debug1
EOF

echo "Starting PostgreSQL..."
if ! pg_ctl -D /data -l /tmp/postgres.log start; then
  echo "ERROR: PostgreSQL failed to start"
  cat /tmp/postgres.log
  exit 1
fi

echo "PostgreSQL is running. Entering interactive shell..."
echo "  - Database data dir: /data"
echo "  - Log file: /tmp/postgres.log"
echo "  - To stop: pg_ctl -D /data stop"
echo ""

exec bash
