#!/bin/bash

#
# Common utilities for bash scripts. Also See Shfile.sh.
#

export PG_SOURCE="/postgres"
export PG_BUILD_DIR="/postgres-build"
export PG_PWH_SOURCE='/pg_what_is_happening'
export PG_DATA_DIR='/data'
export PG_BIN_DIR='/postgres-bin'
export PG_LOG_FILE='/pg_what_is_happening/test/postgresql.log'

export PGDATA="$PG_DATA_DIR"
export PATH="$PATH:$PG_BIN_DIR/bin"
export TERM="xterm" # for GitHub Actions

_log_date()
{
  date "+%Y-%m-%d at %X"
}

_log_red()
{
  echo "$(tput setaf 1)$*$(tput sgr0)"
}

_log_bold()
{
  echo "$(tput bold)$*$(tput sgr0)"
}

log()
{
  _log_bold "$(printf '%s [LOG] %s\n' "$(_log_date)" "$*")" >&2
}

log_err_and_die()
{
  _log_red "$(printf '%s [ERR] %s\n' "$(_log_date)" "$*")" >&2
  exit 1
}

init_env()
{
  log "initializing env"
  mkdir -p "$PG_BUILD_DIR" "$PG_BIN_DIR" "$PG_DATA_DIR"
}

parallel_jobs()
{
  jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
  case $jobs in
  "" | 0 | *[!0-9]*) echo 1 ;;
  *) echo "$jobs" ;;
  esac
}

build_postgresql_if_not_built()
{
  if ! test -f "$PG_BUILD_DIR/config.status"; then
    log "configuring PostgreSQL..."
    cd "$PG_BUILD_DIR" || return
    PG_CFLAGS="-std=gnu11 -g3 -O0 -Wno-error=incompatible-pointer-types"
    "$PG_SOURCE/configure" --prefix="$PG_BIN_DIR" --enable-debug --enable-cassert CFLAGS="$PG_CFLAGS" >/dev/null
    cd - || return
  fi
  log "building PostgreSQL from source..."
  make -C "$PG_BUILD_DIR" -s -j"$(parallel_jobs)" install
}

init_postgresql_data_dir()
{
  log "initializing database..."
  initdb -D "$PG_DATA_DIR" > /dev/null
}

edit_postgresql_conf()
{
  log "editing postgresql.conf..."
  printf '%s\n' \
    "shared_preload_libraries = 'pg_what_is_happening'" \
    "log_min_messages = debug4" \
    "what_is_happening.min_cost_to_track = 0" \
    >> "$PG_DATA_DIR/postgresql.conf"
}

start_postgresql()
{
  log "starting PostgreSQL..."
  : > "$PG_LOG_FILE"
  if ! pg_ctl -D "$PG_DATA_DIR" -l "$PG_LOG_FILE" -w start; then
    cat -n "$PG_LOG_FILE"
    log_err_and_die "PostgreSQL failed to start"
  fi
}
