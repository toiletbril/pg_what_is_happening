#!/bin/sh

set -eu

#
# Containerized makefile. Used to create containers and invoke Nakefile
# commands inside.
#
# Usage:
#   POSTGRES_SOURCE='<path/to/postgres/source>' Shfile.sh <make-image/build/test/dev/reset>
#
# Before everything else, make an image via 'make-image', then either:
#   - Run 'build' to produce static binaries.
#   - Run 'dev' to fall into interactive container for development.
#   - Run 'test' to invoke 'make installcheck' using provided postgres source
#     tree.
#   - Run 'reset' to clean up the image and all build artifacts.
#

IMG='pg-what-is-happening-test'

clean_docker_target() {
  if docker image inspect "$IMG" > /dev/null 2>&1; then
    docker rmi -f "$IMG"
  fi
}

make_sure_postgres_source_is_available() {
  if test -z "${POSTGRES_SOURCE:-}"; then
    echo "ERROR: POSTGRES_SOURCE environment variable must be set to postgres source directory." >&2
    exit 1
  elif ! test -f "$POSTGRES_SOURCE/configure"; then
    echo "ERROR: $POSTGRES_SOURCE does not look like postgres source." >&2
    exit 1
  fi
}

C="${1:-}"

docker_run() {
docker run --pull=never --rm --network=host -v '$PWD:/pg_what_is_happening' \
           -v '$POSTGRES_SOURCE:/postgres' "$@"
}

case $C in
"make-image")
  clean_docker_target
  docker build --network=host --progress=plain -f Dockerfile \
               -t "$IMG" "$(dirname "$0")"
  ;;
"build")
  make_sure_postgres_source_is_available
  BUILD_CMD=$(cat "$(dirname "$0")/scripts/build.sh")
  docker_run "$IMG" sh -c "$BUILD_CMD"
  ;;
"test")
  make_sure_postgres_source_is_available
  TEST_CMD=$(cat "$(dirname "$0")/scripts/test.sh")
  docker_run "$IMG" sh -c "$TEST_CMD"
  ;;
"dev")
  make_sure_postgres_source_is_available
  DEVELOPMENT_CMD=$(cat "$(dirname "$0")/scripts/development.sh")
  docker_run "$IMG" sh -c "$DEVELOPMENT_CMD"
  ;;
"reset")
  echo "Cleaning extension build artifacts..."
  make reset
  if test -n "${POSTGRES_SOURCE:-}" && test -d "$POSTGRES_SOURCE"; then
    make -C "$POSTGRES_SOURCE" -s distclean >/dev/null 2>&1 || true
  fi
  clean_docker_target
  ;;
*)
  echo "USAGE"
  echo "  POSTGRES_SOURCE='<path/to/postgres/source>' $0 <make-image/build/test/dev/reset>"
  exit 1
  ;;
esac
