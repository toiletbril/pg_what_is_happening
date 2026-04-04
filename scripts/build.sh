#!/bin/bash

#
# See Shfile.sh.
#

set -xeu

cd '/pg_what_is_happening'
. "scripts/common.sh"
init_env

echo "Building pg_what_is_happening from source..."
make -j$(nproc) all

ls -lah pg_what_is_happening.so
