#!/usr/bin/env bash
#
# Run all tests: the PostgreSQL regression suite with the patches applied,
# the test module for the hooks, both extensions and the hook chain test.
#
# Run scripts/build-postgres.sh first, with the same PG_VERSION, WORKDIR and
# PREFIX.
#
# On PostgreSQL 18 and later, reads may complete asynchronously, in a way that
# depends on the io_method setting.  So everything except the regression
# suite runs once for each io_method in IO_METHODS (the tests read the setting
# from a TEMP_CONFIG file, which PostgreSQL::Test::Cluster appends to the
# configuration of every server it creates).
#
# Environment variables (all optional):
#   PG_VERSION  as for build-postgres.sh      (default: 19)
#   WORKDIR     as for build-postgres.sh      (default: ./build/$PG_VERSION)
#   PREFIX      as for build-postgres.sh      (default: $WORKDIR/install)
#   SKIP_CORE   set to 1 to skip the PostgreSQL regression suite (about 3
#               minutes); the other tests are much faster
#   IO_METHODS  space-separated io_method values to test with
#               (default: "sync worker", plus "io_uring" if the server was
#               built with liburing; ignored for PostgreSQL 17)
#
# The exit status is non-zero if any step fails.
#
# Usage: scripts/run-tests.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_VERSION="${PG_VERSION:-19}"
WORKDIR="${WORKDIR:-$ROOT/build/$PG_VERSION}"
PREFIX="${PREFIX:-$WORKDIR/install}"
OBJ="$WORKDIR/obj"

if [ ! -x "$PREFIX/bin/pg_config" ]; then
    echo "error: no PostgreSQL in $PREFIX; run scripts/build-postgres.sh first" >&2
    exit 1
fi

# The tests change directories, so PATH needs an absolute path.
PREFIX="$(cd "$PREFIX" && pwd)"
OBJ="$(cd "$OBJ" && pwd)"

# The tests start their own temporary servers; they find the binaries here.
export PATH="$PREFIX/bin:$PATH"
export PG_CONFIG="$PREFIX/bin/pg_config"

installed="$(pg_config --version | sed -E 's/^PostgreSQL ([0-9]+).*/\1/')"
if [ "$installed" != "$PG_VERSION" ]; then
    echo "error: $PREFIX has PostgreSQL $installed, but PG_VERSION is $PG_VERSION" >&2
    exit 1
fi

# The io_method values to test with; an empty string means "the server's
# default" and is used for PostgreSQL 17, which has no such setting.
if [ "$PG_VERSION" -ge 18 ]; then
    if [ -z "${IO_METHODS:-}" ]; then
        IO_METHODS="sync worker"
        if pg_config --configure | grep -q -- '--with-liburing'; then
            IO_METHODS="$IO_METHODS io_uring"
        fi
    fi
    read -r -a methods <<< "$IO_METHODS"
else
    methods=("")
fi

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

# The extensions are built against the installed server and tested with
# "installcheck", which runs the TAP tests against it.
for ext in pg_io_audit pg_page_crypt; do
    dir="$ROOT/extensions/$ext"
    make -C "$dir" clean > /dev/null
    step "$ext: build and install" \
        bash -c "make -C '$dir' && make -C '$dir' install"
done

temp_config="$(mktemp)"
trap 'rm -f "$temp_config"' EXIT

for method in "${methods[@]}"; do
    if [ -n "$method" ]; then
        label=" [io_method=$method]"
        echo "io_method = $method" > "$temp_config"
        export TEMP_CONFIG="$temp_config"
    else
        label=""
        unset TEMP_CONFIG
    fi

    step "test_smgr_hooks (hooks test module)$label" \
        make -C "$OBJ/src/test/modules/test_smgr_hooks" check

    for ext in pg_io_audit pg_page_crypt; do
        step "$ext: tests$label" make -C "$ROOT/extensions/$ext" installcheck
    done

    step "hook chain test (both extensions together)$label" \
        make -C "$ROOT/tests/hook_chain" installcheck
done

echo
if [ ${#failed[@]} -eq 0 ]; then
    echo "All tests passed."
else
    echo "Failed steps:"
    printf '  %s\n' "${failed[@]}"
    exit 1
fi
