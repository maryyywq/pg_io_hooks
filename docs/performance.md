# Performance

How much the hooks and the extensions cost, measured with `pgbench`.

## Summary

| Configuration | Read-only | Read-write |
|---------------|-----------|------------|
| Patched server, no hook set | no measurable difference | no measurable difference |
| `pg_io_audit` | no measurable difference | 0 to −3% |
| `pg_page_crypt` | −9% to −11.5% | −1.5% to −6.5% |

The differences are to PostgreSQL without the patches, over two runs (see
[Results by run](#results-by-run)). "No measurable difference" means within
the noise of the measurement, which is about ±3% from round to round.

- **The hooks themselves** cost nothing measurable when no extension uses
  them, as expected from their design (one test of a NULL pointer per call).
- **`pg_io_audit`** only adds atomic counters in shared memory. On reads this
  is not visible; on the read-write workload it costs up to about 3%.
- **`pg_page_crypt`** encrypts every page that is written and decrypts every
  page that is read from a relation file. With data larger than
  `shared_buffers`, this happens all the time, and costs about 10% of the
  read-only throughput. On the read-write workload the share is smaller,
  because time spent on WAL and flushing does not change; it depends on how
  much the machine is limited by its disk (see the two runs).

## Setup

PostgreSQL 19.0, release builds (`-O2`, no assertions), data checksums on.
`pgbench` scale 100 (about 1.5 GB) against `shared_buffers = 128MB`, so pages
constantly move between shared buffers and the relation files, through the
hooks; the files stay in the page cache of the operating system, so the CPU
cost is measured rather than disk speed. 4 clients, `io_method = worker`.
Two runs of the `.github/workflows/bench.yml` workflow on GitHub-hosted
`ubuntu-24.04` runners (4 CPUs, 16 GB), chosen over a laptop for their stable
CPU speed.

Four configurations are compared: `vanilla` (the base commit without the
patches), `hooks` (patched, no extension), `audit` (`pg_io_audit` loaded) and
`crypt` (`pg_page_crypt` loaded, cluster converted). A round runs each of
them with a read-only test (`pgbench -S`) and the default read-write test,
60 seconds each; there are 5 rounds. Each configuration is compared with
`vanilla` of the same round, and the median over the rounds is reported.

## Results by run

Median difference to `vanilla`:

| Configuration | Read-only, run 1 | Read-only, run 2 | Read-write, run 1 | Read-write, run 2 |
|---------------|-----------------:|-----------------:|------------------:|------------------:|
| `hooks` | +1.3% | +0.2% | −1.4% | +0.2% |
| `audit` | +0.6% | −0.0% | −3.0% | −0.2% |
| `crypt` | −11.5% | −9.0% | −6.5% | −1.5% |

The runner of run 2 had slower storage: its `vanilla` read-write throughput
was 4542 tps against 10663 tps in run 1 (read-only: 84147 against 112034).
With more time spent waiting for the disk, the relative cost of encryption on
the read-write test is smaller.

## Running it

On a machine whose speed is stable:

```
BUILD_TYPE=release WORKDIR=build/19-release scripts/build-postgres.sh
BUILD_TYPE=release APPLY_PATCHES=no WORKDIR=build/19-vanilla scripts/build-postgres.sh
scripts/bench.sh
```

The variables at the top of `scripts/bench.sh` change the scale, the
duration, the number of rounds and clients. Results go to
`build/bench/results.csv` (one line per test) and the summary to standard
output. On GitHub, run the "Benchmark" workflow from the Actions tab; it
takes about an hour with the defaults, and the results are in the job
summary and the `bench-results` artifact.

## Not measured

- PostgreSQL 17 and 18, other `io_method` values (`sync`, `io_uring`), other
  `shared_buffers` sizes and larger data that does not fit into the page
  cache, where disk time would dominate and the relative cost of encryption
  would be lower.
- Latency distributions; only throughput is compared.
- The time of `pg_page_crypt_convert` on large clusters.
