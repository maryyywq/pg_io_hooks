#!/usr/bin/env bash
#
# Build and install PostgreSQL with the smgr hooks patch series applied.
#
# What it does:
#   1. fetches the PostgreSQL commit that the patches for $PG_VERSION are
#      based on (named in patches/$PG_VERSION/0001-*.patch as "base-commit:")
#      into $WORKDIR/postgres,
#   2. applies the patches with "git am",
#   3. configures an out-of-tree build in $WORKDIR/obj,
#   4. builds and installs into $PREFIX.
#
# It is safe to run again: an already prepared source tree is reused.  To
# start from scratch, remove $WORKDIR.
#
# Environment variables (all optional):
#   PG_VERSION  PostgreSQL major version: 17, 18 or 19 (default: 19)
#   WORKDIR     where sources and build files go       (default: ./build/$PG_VERSION)
#   PREFIX      installation directory                 (default: $WORKDIR/install)
#   PG_REPO     where to fetch PostgreSQL from         (default: GitHub mirror)
#   BUILD_TYPE  "debug" (assertions on, for testing) or "release"
#               (optimized, for measurements)          (default: debug)
#   JOBS        parallel build jobs                    (default: all CPUs)
#   APPLY_PATCHES  "yes" or "no": "no" builds the same base commit without
#               the patches, for comparisons (see scripts/bench.sh)
#                                                      (default: yes)
#   LIBURING    "auto", "yes" or "no": build with liburing, needed for
#               io_method = io_uring (PostgreSQL 18 and later)
#                                                      (default: auto, i.e.
#                                                      if it is installed)
#
# Usage: scripts/build-postgres.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_VERSION="${PG_VERSION:-19}"
WORKDIR="${WORKDIR:-$ROOT/build/$PG_VERSION}"
PREFIX="${PREFIX:-$WORKDIR/install}"
PG_REPO="${PG_REPO:-https://github.com/postgres/postgres.git}"
BUILD_TYPE="${BUILD_TYPE:-debug}"
JOBS="${JOBS:-$(nproc)}"
LIBURING="${LIBURING:-auto}"
APPLY_PATCHES="${APPLY_PATCHES:-yes}"

# The build changes directories, so relative paths would break.
mkdir -p "$WORKDIR" "$PREFIX"
WORKDIR="$(cd "$WORKDIR" && pwd)"
PREFIX="$(cd "$PREFIX" && pwd)"

PATCHES="$ROOT/patches/$PG_VERSION"
SRC="$WORKDIR/postgres"
OBJ="$WORKDIR/obj"

if ! ls "$PATCHES"/0001-*.patch > /dev/null 2>&1; then
    echo "error: no patches for PostgreSQL $PG_VERSION in $PATCHES" >&2
    exit 1
fi

# The commit the patches apply to, recorded by "git format-patch --base".
BASE="$(sed -n 's/^base-commit: //p' "$PATCHES"/0001-*.patch | head -n 1)"
if [ -z "$BASE" ]; then
    echo "error: no base-commit found in $PATCHES/0001-*.patch" >&2
    exit 1
fi

echo "==> PostgreSQL $PG_VERSION, base commit: $BASE"

# A checksum of the patch files, stored in the source tree after they are
# applied, so that a later run notices when the patches have changed.
case "$APPLY_PATCHES" in
    yes) PATCHES_SUM="$(cat "$PATCHES"/*.patch | sha256sum | cut -d' ' -f1)" ;;
    no)  PATCHES_SUM="none" ;;
    *)
        echo "error: APPLY_PATCHES must be 'yes' or 'no'" >&2
        exit 1
        ;;
esac
STAMP="$SRC/.pg_io_hooks-patches.sha256"

if [ ! -d "$SRC/.git" ]; then
    echo "==> Fetching PostgreSQL source from $PG_REPO"
    mkdir -p "$SRC"
    git -C "$SRC" init --quiet
    git -C "$SRC" remote add origin "$PG_REPO"
    git -C "$SRC" fetch --quiet --depth 1 origin "$BASE"
    git -C "$SRC" checkout --quiet --detach FETCH_HEAD

    if [ "$APPLY_PATCHES" = yes ]; then
        echo "==> Applying patches"
        # "git am" needs an identity for the commits it creates in this
        # throw-away source tree; the patches keep their own authors.
        git -C "$SRC" -c user.name="pg_io_hooks build" -c user.email="build@localhost" \
            am --quiet "$PATCHES"/*.patch
    else
        echo "==> Not applying the patches (APPLY_PATCHES=no)"
    fi
    echo "$PATCHES_SUM" > "$STAMP"
else
    echo "==> Reusing the source tree in $SRC"
    if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$PATCHES_SUM" ]; then
        echo "error: the source tree in $SRC was not prepared from the current patches" >&2
        echo "       (or with another APPLY_PATCHES setting);" >&2
        echo "       remove $WORKDIR and run this script again" >&2
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

# io_method = io_uring (PostgreSQL 18 and later) needs liburing.
if [ "$PG_VERSION" -ge 18 ]; then
    case "$LIBURING" in
        auto)
            if pkg-config --exists liburing 2> /dev/null; then
                CONFIGURE_FLAGS+=(--with-liburing)
            else
                echo "==> liburing not found, building without io_method = io_uring"
            fi
            ;;
        yes)
            CONFIGURE_FLAGS+=(--with-liburing)
            ;;
        no)
            ;;
        *)
            echo "error: LIBURING must be 'auto', 'yes' or 'no'" >&2
            exit 1
            ;;
    esac
fi

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
echo "Run the tests with scripts/run-tests.sh (use the same PG_VERSION, WORKDIR and PREFIX)."
