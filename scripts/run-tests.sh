#!/usr/bin/env bash
#
# Run all tests: the PostgreSQL regression suite with the patches applied,
# the test module for the hooks, both extensions and the hook chain test.
#
# Run scripts/build-postgres.sh first, with the same WORKDIR and PREFIX.
#
# Environment variables (all optional):
#   WORKDIR     as for build-postgres.sh      (default: ./build)
#   PREFIX      as for build-postgres.sh      (default: $WORKDIR/install)
#   SKIP_CORE   set to 1 to skip the PostgreSQL regression suite (about 3
#               minutes); the other tests are much faster
#
# The exit status is non-zero if any step fails.
#
# Usage: scripts/run-tests.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKDIR="${WORKDIR:-$ROOT/build}"
PREFIX="${PREFIX:-$WORKDIR/install}"
OBJ="$WORKDIR/obj"

if [ ! -x "$PREFIX/bin/pg_config" ]; then
    echo "error: no PostgreSQL in $PREFIX; run scripts/build-postgres.sh first" >&2
    exit 1
fi

# The tests start their own temporary servers; they find the binaries here.
export PATH="$PREFIX/bin:$PATH"
export PG_CONFIG="$PREFIX/bin/pg_config"

failed=()

step() {
    local name="$1"
    shift
    echo
    echo "==> $name"
    if "$@"; then
        echo "--> $name: ok"
    else
        echo "--> $name: FAILED"
        failed+=("$name")
    fi
}

if [ "${SKIP_CORE:-0}" != "1" ]; then
    step "PostgreSQL regression suite (make check)" make -C "$OBJ" check
fi

step "test_smgr_hooks (hooks test module)" \
    make -C "$OBJ/src/test/modules/test_smgr_hooks" check

# The extensions are built against the installed server and tested with
# "installcheck", which runs the TAP tests against it.
for ext in pg_io_audit pg_page_crypt; do
    dir="$ROOT/extensions/$ext"
    make -C "$dir" clean > /dev/null
    step "$ext: build and install" \
        bash -c "make -C '$dir' && make -C '$dir' install"
    step "$ext: tests" make -C "$dir" installcheck
done

step "hook chain test (both extensions together)" \
    make -C "$ROOT/tests/hook_chain" installcheck

echo
if [ ${#failed[@]} -eq 0 ]; then
    echo "All tests passed."
else
    echo "Failed steps:"
    printf '  %s\n' "${failed[@]}"
    exit 1
fi
