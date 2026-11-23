#!/usr/bin/env bash
#
# Test pg_upgrade of a cluster encrypted with pg_page_crypt, from an older
# PostgreSQL version to a newer one (tests/upgrade).
#
# Build both versions with scripts/build-postgres.sh first, for example
#   PG_VERSION=17 scripts/build-postgres.sh
#   PG_VERSION=19 scripts/build-postgres.sh
# This script installs pg_page_crypt into both and runs the test.
#
# Environment variables (all optional):
#   OLD_VERSION  version to upgrade from                (default: 17)
#   NEW_VERSION  version to upgrade to                  (default: 19)
#   OLD_PREFIX   installation of OLD_VERSION            (default: ./build/$OLD_VERSION/install)
#   NEW_PREFIX   installation of NEW_VERSION            (default: ./build/$NEW_VERSION/install)
#
# Usage: scripts/run-upgrade-test.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OLD_VERSION="${OLD_VERSION:-17}"
NEW_VERSION="${NEW_VERSION:-19}"
OLD_PREFIX="${OLD_PREFIX:-$ROOT/build/$OLD_VERSION/install}"
NEW_PREFIX="${NEW_PREFIX:-$ROOT/build/$NEW_VERSION/install}"

for v in OLD NEW; do
    prefix_var="${v}_PREFIX"
    version_var="${v}_VERSION"
    prefix="${!prefix_var}"
    if [ ! -x "$prefix/bin/pg_config" ]; then
        echo "error: no PostgreSQL in $prefix; run scripts/build-postgres.sh first" >&2
        exit 1
    fi
    installed="$("$prefix/bin/pg_config" --version | sed -E 's/^PostgreSQL ([0-9]+).*/\1/')"
    if [ "$installed" != "${!version_var}" ]; then
        echo "error: $prefix has PostgreSQL $installed, but $version_var is ${!version_var}" >&2
        exit 1
    fi
done

# The tests change directories, so they need absolute paths.
OLD_PREFIX="$(cd "$OLD_PREFIX" && pwd)"
NEW_PREFIX="$(cd "$NEW_PREFIX" && pwd)"

# The same source directory is built for each version in turn; build the new
# version last, so that it is what stays there.
ext="$ROOT/extensions/pg_page_crypt"
for prefix in "$OLD_PREFIX" "$NEW_PREFIX"; do
    echo "==> pg_page_crypt: build and install into $prefix"
    make -C "$ext" PG_CONFIG="$prefix/bin/pg_config" clean > /dev/null
    make -C "$ext" PG_CONFIG="$prefix/bin/pg_config" > /dev/null
    make -C "$ext" PG_CONFIG="$prefix/bin/pg_config" install > /dev/null
done

echo "==> pg_upgrade from PostgreSQL $OLD_VERSION to $NEW_VERSION"
# The test runs the new version's tools from PATH; settings for the servers
# of other tests (TEMP_CONFIG, such as an io_method) do not apply here.
unset TEMP_CONFIG
PATH="$NEW_PREFIX/bin:$PATH" OLD_INSTALL="$OLD_PREFIX" \
    make -C "$ROOT/tests/upgrade" PG_CONFIG="$NEW_PREFIX/bin/pg_config" installcheck
