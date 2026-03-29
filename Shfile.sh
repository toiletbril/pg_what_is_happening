#!/bin/sh

set -eu

IMG='pg-what-is-happening-test'

BUILD_CMD='
set -eu

cd /pg_what_is_happening

if ! test -f /postgres/config.status; then
  echo "Configuring PostgreSQL..."
  cd /postgres
  ./configure --prefix=/postgres-bin --enable-debug --enable-cassert CFLAGS="-g3 -O0" >/dev/null
  cd -
fi

echo "Building PostgreSQL from source..."
make -C /postgres -s -j$(nproc) install
export PATH=/postgres-bin/bin:$PATH

echo "Building pg_what_is_happening from source..."
make -C /pg_what_is_happening -j$(nproc) all

echo "Build completed successfully"

ls -lah pg_what_is_happening.so
'

TEST_CMD='
set -eu

cd /pg_what_is_happening

if ! test -f /postgres/config.status; then
  echo "Configuring PostgreSQL..."
  cd /postgres
  ./configure --prefix=/postgres-bin --enable-debug --enable-cassert CFLAGS="-g3 -O0" >/dev/null
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
max_connections = 50
shared_preload_libraries = 'pg_what_is_happening'
log_min_messages = debug1
EOF

if ! pg_ctl -D /data -l /tmp/postgres.log start; then
  echo "ERROR: PostgreSQL failed to start"
  cat /tmp/postgres.log
  exit 1
fi

if ! make -C /pg_what_is_happening installcheck; then
  echo "ERROR: Tests failed"
  cat /tmp/postgres.log
  pg_ctl -D /data stop
  exit 1
fi

pg_ctl -D /data stop
'

DEV_CMD='
set -eu

cd /pg_what_is_happening

if ! test -f /postgres/config.status; then
  echo "Configuring PostgreSQL..."
  cd /postgres
  ./configure --prefix=/postgres-bin --enable-debug --enable-cassert CFLAGS="-g3 -O0" >/dev/null
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
'

clean_docker_target() {
  if docker image inspect "$IMG" > /dev/null 2>&1; then
    docker rmi -f "$IMG"
  fi
}

require_pg_src() {
  if test -z "${POSTGRES_SOURCE:-}"; then
    echo "ERROR: POSTGRES_SOURCE environment variable must be set to postgres source directory." >&2
    exit 1
  elif ! test -f "$POSTGRES_SOURCE/configure"; then
    echo "ERROR: $POSTGRES_SOURCE does not look like postgres source." >&2
    exit 1
  fi
}

C="${1:-}"

case $C in
"make-image")
  clean_docker_target
  docker build --network=host --progress=plain -f Dockerfile \
               -t "$IMG" "$(dirname "$0")"
  ;;
"build")
  require_pg_src
  docker run --pull=never --rm --network=host \
             --user "$(id -u):$(id -g)" \
             -e MODE="${MODE:-rel}" \
             -e WITH_BGWORKER="${WITH_BGWORKER:-yes}" \
             -v "$PWD":/pg_what_is_happening \
             -v "$POSTGRES_SOURCE":/postgres \
             "$IMG" sh -c "$BUILD_CMD"
  ;;
"test")
  require_pg_src
  docker run --pull=never --rm --network=host \
             --user "$(id -u):$(id -g)" \
             -e MODE="${MODE:-rel}" \
             -e WITH_BGWORKER="${WITH_BGWORKER:-yes}" \
             -v "$PWD":/pg_what_is_happening \
             -v "$POSTGRES_SOURCE":/postgres \
             "$IMG" sh -c "$TEST_CMD"
  ;;
"dev")
  require_pg_src
  docker run --pull=never --rm --network=host -it \
             --user "$(id -u):$(id -g)" \
             -e MODE="${MODE:-dbg}" \
             -e WITH_BGWORKER="${WITH_BGWORKER:-yes}" \
             -v "$PWD":/pg_what_is_happening \
             -v "$POSTGRES_SOURCE":/postgres \
             "$IMG" sh -c "$DEV_CMD"
  ;;
"reset")
  echo "Cleaning extension build artifacts..."
  rm -f pg_what_is_happening.so
  make reset

  if test -n "${POSTGRES_SOURCE:-}" && test -d "$POSTGRES_SOURCE"; then
    echo "Cleaning postgres source directory..."
    make -C "$POSTGRES_SOURCE" -s distclean >/dev/null 2>&1 || true
  fi

  echo "Removing docker image..."
  clean_docker_target
  ;;
*)
  echo "USAGE"
  echo "  POSTGRES_SOURCE='<path/to/postgres/source>' $0 <make-image/build/test/dev/reset>"
  exit 1
  ;;
esac
