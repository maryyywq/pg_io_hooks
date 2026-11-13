#!/usr/bin/env bash
#
# Build and install PostgreSQL 17 with the smgr hooks patch series applied.
#
# What it does:
#   1. fetches the PostgreSQL commit that the patches are based on (named in
#      patches/0001-*.patch as "base-commit:") into $WORKDIR/postgres,
#   2. applies the patches with "git am",
#   3. configures an out-of-tree build in $WORKDIR/obj,
#   4. builds and installs into $PREFIX.
#
# It is safe to run again: an already prepared source tree is reused.  To
# start from scratch, remove $WORKDIR.
#
# Environment variables (all optional):
#   WORKDIR     where sources and build files go       (default: ./build)
#   PREFIX      installation directory                 (default: $WORKDIR/install)
#   PG_REPO     where to fetch PostgreSQL from         (default: GitHub mirror)
#   BUILD_TYPE  "debug" (assertions on, for testing) or "release"
#               (optimized, for measurements)          (default: debug)
#   JOBS        parallel build jobs                    (default: all CPUs)
#
# Usage: scripts/build-postgres.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKDIR="${WORKDIR:-$ROOT/build}"
PREFIX="${PREFIX:-$WORKDIR/install}"
PG_REPO="${PG_REPO:-https://github.com/postgres/postgres.git}"
BUILD_TYPE="${BUILD_TYPE:-debug}"
JOBS="${JOBS:-$(nproc)}"

SRC="$WORKDIR/postgres"
OBJ="$WORKDIR/obj"

# The commit the patches apply to, recorded by "git format-patch --base".
BASE="$(sed -n 's/^base-commit: //p' "$ROOT"/patches/0001-*.patch | head -n 1)"
if [ -z "$BASE" ]; then
    echo "error: no base-commit found in patches/0001-*.patch" >&2
    exit 1
fi

echo "==> PostgreSQL base commit: $BASE"

if [ ! -d "$SRC/.git" ]; then
    echo "==> Fetching PostgreSQL source from $PG_REPO"
    mkdir -p "$SRC"
    git -C "$SRC" init --quiet
    git -C "$SRC" remote add origin "$PG_REPO"
    git -C "$SRC" fetch --quiet --depth 1 origin "$BASE"
    git -C "$SRC" checkout --quiet --detach FETCH_HEAD

    echo "==> Applying patches"
    # "git am" needs an identity for the commits it creates in this
    # throw-away source tree; the patches keep their own authors.
    git -C "$SRC" -c user.name="pg_io_hooks build" -c user.email="build@localhost" \
        am --quiet "$ROOT"/patches/*.patch
else
    echo "==> Reusing the source tree in $SRC"
    if ! grep -q smgr_read_hook "$SRC/src/include/storage/smgr.h"; then
        echo "error: $SRC exists but does not contain the patches; remove $WORKDIR and run again" >&2
        exit 1
    fi
fi

case "$BUILD_TYPE" in
    debug)
        CONFIGURE_FLAGS=(--enable-debug --enable-cassert)
        CFLAGS="-O1"
        ;;
    release)
        CONFIGURE_FLAGS=()
        CFLAGS="-O2"
        ;;
    *)
        echo "error: BUILD_TYPE must be 'debug' or 'release'" >&2
        exit 1
        ;;
esac

echo "==> Configuring ($BUILD_TYPE build) in $OBJ"
mkdir -p "$OBJ"
(
    cd "$OBJ"
    "$SRC/configure" \
        --prefix="$PREFIX" \
        --enable-tap-tests \
        --with-openssl \
        "${CONFIGURE_FLAGS[@]}" \
        CFLAGS="$CFLAGS" \
        > configure.log
)

echo "==> Building with $JOBS jobs"
make -C "$OBJ" -j "$JOBS" > "$OBJ/make.log"

echo "==> Installing into $PREFIX"
make -C "$OBJ" install > "$OBJ/install.log"

echo
echo "Done. PostgreSQL is installed in $PREFIX"
echo "  export PATH=\"$PREFIX/bin:\$PATH\""
echo "Run the tests with scripts/run-tests.sh (use the same WORKDIR and PREFIX)."
