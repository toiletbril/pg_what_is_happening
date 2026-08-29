#!/bin/sh

set -eu

host_uid=${PWH_HOST_UID:-1000}
host_gid=${PWH_HOST_GID:-1000}

group_name=$(getent group "$host_gid" | cut -d: -f1 || true)
if test -z "$group_name"; then
  group_name=pwh_host
  addgroup -g "$host_gid" "$group_name"
fi

user_name=$(getent passwd "$host_uid" | cut -d: -f1 || true)
if test -z "$user_name"; then
  adduser -D -H -u "$host_uid" -G "$group_name" pwh_host
fi

exec su-exec "$host_uid:$host_gid" "$@"
