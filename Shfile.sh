#!/bin/sh

set -eu

IMG='pg-what-is-happening-test'

BUILD_CMD=$(cat "$(dirname "$0")/scripts/build.sh")
TEST_CMD=$(cat "$(dirname "$0")/scripts/test.sh")
DEV_CMD=$(cat "$(dirname "$0")/scripts/dev.sh")

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
