#!/usr/bin/env bash
#
# Measure the overhead of the smgr hooks and of the extensions with pgbench.
#
# Compares four configurations of the same PostgreSQL version:
#   vanilla   PostgreSQL without the patches
#   hooks     with the patches, no extension loaded (all hooks unset)
#   audit     with the patches and pg_io_audit
#   crypt     with the patches and pg_page_crypt (cluster converted)
#
# Each configuration gets its own cluster with the same pgbench data, larger
# than shared_buffers, so that pages are read and written through the
# storage manager all the time.  The rounds run the configurations in turn
# (vanilla, hooks, audit, crypt, vanilla, ...), so that slow drifts of the
# machine affect all of them alike.  Each round runs a read-only and a
# read-write pgbench test.
#
# Both servers should be release builds:
#   BUILD_TYPE=release WORKDIR=build/19-release scripts/build-postgres.sh
#   BUILD_TYPE=release APPLY_PATCHES=no WORKDIR=build/19-vanilla \
#       scripts/build-postgres.sh
#
# Environment variables (all optional):
#   PATCHED_PREFIX  installation with the patches (default: build/19-release/install)
#   VANILLA_PREFIX  installation without them     (default: build/19-vanilla/install)
#   BENCHDIR        where clusters and results go (default: build/bench)
#   SCALE           pgbench scale factor          (default: 100, about 1.5 GB)
#   SHARED_BUFFERS  shared_buffers                (default: 128MB)
#   CLIENTS         pgbench clients and threads   (default: number of CPUs)
#   DURATION        seconds per pgbench run       (default: 60)
#   ROUNDS          number of rounds              (default: 5)
#   PORT            port of the test servers      (default: 5499)
#
# Laptops and shared machines change speed under sustained load (thermal
# throttling, power management, neighbours).  Before each pgbench run the
# script times a fixed CPU-bound loop; the time is stored with the result,
# and the summary warns when it varies by more than 30%, because the numbers
# are then not comparable.
#
# Results: $BENCHDIR/results.csv (one line per run) and a summary table on
# standard output, in Markdown.
#
# Usage: scripts/bench.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCHED_PREFIX="${PATCHED_PREFIX:-$ROOT/build/19-release/install}"
VANILLA_PREFIX="${VANILLA_PREFIX:-$ROOT/build/19-vanilla/install}"
BENCHDIR="${BENCHDIR:-$ROOT/build/bench}"
SCALE="${SCALE:-100}"
SHARED_BUFFERS="${SHARED_BUFFERS:-128MB}"
CLIENTS="${CLIENTS:-$(nproc)}"
DURATION="${DURATION:-60}"
ROUNDS="${ROUNDS:-5}"
PORT="${PORT:-5499}"

CONFIGS=(vanilla hooks audit crypt)

# Unix-domain socket paths are limited to about 100 bytes, so the sockets go
# to a short temporary directory rather than to $BENCHDIR.
SOCKDIR="$(mktemp -d /tmp/pg_io_hooks_bench.XXXXXX)"
trap 'rm -rf "$SOCKDIR"' EXIT
RESULTS="$BENCHDIR/results.csv"
KEY="$BENCHDIR/pg_page_crypt.key"

for prefix in "$PATCHED_PREFIX" "$VANILLA_PREFIX"; do
    if [ ! -x "$prefix/bin/pg_config" ]; then
        echo "error: no PostgreSQL in $prefix (see the comments in this script)" >&2
        exit 1
    fi
done

prefix_of() {
    if [ "$1" = vanilla ]; then echo "$VANILLA_PREFIX"; else echo "$PATCHED_PREFIX"; fi
}

# Run a command with the binaries of a configuration first in PATH.
with_bin() {
    local cfg="$1"
    shift
    PATH="$(prefix_of "$cfg")/bin:$PATH" "$@"
}

pgdata() { echo "$BENCHDIR/$1"; }

start() {
    with_bin "$1" pg_ctl -D "$(pgdata "$1")" -l "$BENCHDIR/$1.log" -w start > /dev/null
}

stop() {
    with_bin "$1" pg_ctl -D "$(pgdata "$1")" -m fast -w stop > /dev/null
}

psql_cmd() {
    local cfg="$1"
    shift
    with_bin "$cfg" psql -X -q -h "$SOCKDIR" -p "$PORT" -d postgres -v ON_ERROR_STOP=1 "$@"
}

# Create the cluster of one configuration with the pgbench data.
setup() {
    local cfg="$1" dir
    dir="$(pgdata "$cfg")"
    echo "==> Setting up $cfg"
    rm -rf "$dir"
    # Data checksums are on in all configurations; pg_page_crypt needs them.
    with_bin "$cfg" initdb -D "$dir" --data-checksums -U postgres -A trust > /dev/null
    cat >> "$dir/postgresql.conf" <<CONF
port = $PORT
listen_addresses = ''
unix_socket_directories = '$SOCKDIR'
shared_buffers = $SHARED_BUFFERS
max_wal_size = 8GB
checkpoint_timeout = 30min
max_connections = $((CLIENTS + 10))
CONF
    start "$cfg"
    with_bin "$cfg" pgbench -i -q -s "$SCALE" -h "$SOCKDIR" -p "$PORT" -U postgres postgres > /dev/null 2>&1
    psql_cmd "$cfg" -U postgres -c "VACUUM ANALYZE" -c "CHECKPOINT"
    stop "$cfg"

    case "$cfg" in
        audit)
            echo "shared_preload_libraries = 'pg_io_audit'" >> "$dir/postgresql.conf"
            ;;
        crypt)
            with_bin "$cfg" pg_page_crypt_convert --encrypt -D "$dir" --key-file "$KEY" > /dev/null
            cat >> "$dir/postgresql.conf" <<CONF
shared_preload_libraries = 'pg_page_crypt'
pg_page_crypt.key_file = '$KEY'
CONF
            ;;
    esac
}

# Time a fixed CPU-bound loop, in milliseconds.
calibrate() {
    local start end
    start="$(date +%s%N)"
    awk 'BEGIN { for (i = 0; i < 20000000; i++) s += i }'
    end="$(date +%s%N)"
    echo $(((end - start) / 1000000))
}

# Run pgbench and print the tps.
run_pgbench() {
    local cfg="$1"
    shift
    with_bin "$cfg" pgbench -n -c "$CLIENTS" -j "$CLIENTS" -T "$DURATION" \
        -h "$SOCKDIR" -p "$PORT" -U postgres "$@" postgres 2> /dev/null |
        sed -n 's/^tps = \([0-9.]*\) .*/\1/p'
}

mkdir -p "$BENCHDIR"
chmod 700 "$BENCHDIR"

echo "==> Installing the extensions into $PATCHED_PREFIX"
for ext in pg_io_audit pg_page_crypt; do
    make -s -C "$ROOT/extensions/$ext" PG_CONFIG="$PATCHED_PREFIX/bin/pg_config" clean > /dev/null
    make -s -C "$ROOT/extensions/$ext" PG_CONFIG="$PATCHED_PREFIX/bin/pg_config" install > /dev/null
done

(umask 077 && openssl rand -hex 64 > "$KEY")

for cfg in "${CONFIGS[@]}"; do
    setup "$cfg"
done

echo "config,workload,round,tps,calibration_ms" > "$RESULTS"
for round in $(seq 1 "$ROUNDS"); do
    for cfg in "${CONFIGS[@]}"; do
        echo "==> Round $round/$ROUNDS: $cfg"
        start "$cfg"
        # Read-only (SELECT only), then the default TPC-B-like read-write test.
        calib="$(calibrate)"
        echo "$cfg,read-only,$round,$(run_pgbench "$cfg" -S),$calib" >> "$RESULTS"
        calib="$(calibrate)"
        echo "$cfg,read-write,$round,$(run_pgbench "$cfg"),$calib" >> "$RESULTS"
        stop "$cfg"
    done
done

# Summary.  The speed of a machine drifts a little between rounds, which
# affects all configurations of a round alike, so each configuration is
# compared with vanilla of the same round; the table shows the median of
# these per-round differences and their range, and the median tps.
echo
echo "PostgreSQL $(with_bin hooks pg_config --version | sed 's/PostgreSQL //'), scale $SCALE," \
     "shared_buffers $SHARED_BUFFERS, $CLIENTS clients, ${DURATION}s x $ROUNDS rounds"
echo "Machine: $(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -n 1)," \
     "$(nproc) CPUs, $(awk '/^MemTotal/ { printf "%.1f GB", $2 / 1048576 }' /proc/meminfo) RAM," \
     "Linux $(uname -r)"
echo
awk -F, '
    NR > 1 {
        tps[$1 "," $2 "," $3] = $4
        if ($3 > rounds) rounds = $3
        if (cmin == "" || $5 < cmin) cmin = $5
        if ($5 > cmax) cmax = $5
    }
    function median(a, n,    i, j, t) {
        for (i = 1; i <= n; i++)
            for (j = i + 1; j <= n; j++)
                if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t }
        return (n % 2) ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
    }
    END {
        split("vanilla hooks audit crypt", cfgs, " ")
        split("read-only read-write", wls, " ")
        print "| Configuration | Workload | Median tps | vs. vanilla (median) | vs. vanilla (range) |"
        print "|---------------|----------|-----------:|---------------------:|--------------------:|"
        for (w = 1; w <= 2; w++) {
            for (c = 1; c <= 4; c++) {
                lo = ""; hi = ""
                for (r = 1; r <= rounds; r++) {
                    t[r] = tps[cfgs[c] "," wls[w] "," r]
                    d[r] = (t[r] / tps["vanilla," wls[w] "," r] - 1) * 100
                    if (lo == "" || d[r] < lo) lo = d[r]
                    if (hi == "" || d[r] > hi) hi = d[r]
                }
                m = median(t, rounds)
                if (cfgs[c] == "vanilla")
                    printf "| %s | %s | %.0f | | |\n", cfgs[c], wls[w], m
                else
                    printf "| %s | %s | %.0f | %+.1f%% | %+.1f%% .. %+.1f%% |\n", cfgs[c], wls[w], m, median(d, rounds), lo, hi
            }
        }
        printf "\nCPU calibration: %d to %d ms", cmin, cmax
        if (cmax > cmin * 1.3)
            printf " -- WARNING: the speed of the machine varied by more than 30%%, the results are not reliable"
        printf "\n"
    }' "$RESULTS"
