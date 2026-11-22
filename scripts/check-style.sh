#!/usr/bin/env bash
#
# Check the code style:
#   - pgindent over the core files that the patches change, in the patched
#     source tree that build-postgres.sh prepared,
#   - pgindent over the C sources of the extensions (their own types are
#     listed in extensions/typedefs.list), with the pgindent of the newest
#     supported version only: versions of pg_bsd_indent differ in details
#     (for example the space in "fmt, ..."), and one source must satisfy
#     one of them,
#   - shellcheck over the scripts.
#
# Run scripts/build-postgres.sh first, with the same PG_VERSION and WORKDIR:
# pgindent and pg_bsd_indent come from that source tree, so that the style
# is the one of the PostgreSQL version.  Prints what pgindent would change.
#
# Environment variables (all optional):
#   PG_VERSION  as for build-postgres.sh      (default: 19)
#   WORKDIR     as for build-postgres.sh      (default: ./build/$PG_VERSION)
#
# Usage: scripts/check-style.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_VERSION="${PG_VERSION:-19}"
WORKDIR="${WORKDIR:-$ROOT/build/$PG_VERSION}"
SRC="$WORKDIR/postgres"
OBJ="$WORKDIR/obj"

if [ ! -d "$SRC/.git" ] || [ ! -d "$OBJ" ]; then
    echo "error: no prepared source tree in $WORKDIR; run scripts/build-postgres.sh first" >&2
    exit 1
fi
SRC="$(cd "$SRC" && pwd)"
OBJ="$(cd "$OBJ" && pwd)"

if ! command -v shellcheck > /dev/null; then
    echo "error: shellcheck is not installed (see scripts/install-deps-debian.sh)" >&2
    exit 1
fi

failed=0

# pgindent needs pg_bsd_indent of the matching version, built in the tree.
make -s -C "$OBJ/src/tools/pg_bsd_indent" > /dev/null
export PATH="$OBJ/src/tools/pg_bsd_indent:$PATH"

typedefs="$(mktemp)"
trap 'rm -f "$typedefs"' EXIT
cat "$SRC/src/tools/pgindent/typedefs.list" "$ROOT/extensions/typedefs.list" > "$typedefs"

# Run pgindent in diff mode; any output means the files are not formatted.
# Returns non-zero in that case.
pgindent_check() {
    local name="$1"
    shift
    local out
    out="$("$SRC/src/tools/pgindent/pgindent" --typedefs="$typedefs" --diff "$@")"
    if [ -n "$out" ]; then
        echo "--> $name: pgindent would change:"
        echo "$out"
        return 1
    fi
    echo "--> $name: ok"
}

BASE="$(sed -n 's/^base-commit: //p' "$ROOT"/patches/"$PG_VERSION"/0001-*.patch | head -n 1)"
mapfile -t core_files < <(git -C "$SRC" diff --name-only "$BASE" HEAD -- '*.c' '*.h')
(cd "$SRC" && pgindent_check "core files changed by the patches" "${core_files[@]}") || failed=1

NEWEST=19
if [ "$PG_VERSION" = "$NEWEST" ]; then
    mapfile -t ext_files < <(cd "$ROOT" && ls extensions/*/*.c extensions/*/*.h)
    (cd "$ROOT" && pgindent_check "extensions" "${ext_files[@]}") || failed=1
else
    echo "--> extensions: skipped, checked with PostgreSQL $NEWEST only"
fi

if shellcheck "$ROOT"/scripts/*.sh; then
    echo "--> shellcheck: ok"
else
    echo "--> shellcheck: FAILED"
    failed=1
fi

exit "$failed"
