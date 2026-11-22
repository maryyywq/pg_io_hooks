# Performance

How much the hooks and the extensions cost, measured with `pgbench`.

## Summary

| Configuration | Read-only | Read-write |
|---------------|-----------|------------|
| Patched server, no hook set | no measurable difference | no measurable difference |
| `pg_io_audit` | no measurable difference | 0 to −3% |
| `pg_page_crypt` | −9% to −11.5% | −1.5% to −6.5% |

The differences are to PostgreSQL without the patches, as medians over the
rounds of two runs (details below). "No measurable difference" means within
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

## Method

`scripts/bench.sh` compares four configurations of the same PostgreSQL
version, all release builds (`-O2`, no assertions), with data checksums on:

| Configuration | Server | Loaded |
|---------------|--------|--------|
| `vanilla` | the base commit of the patches, without them (`APPLY_PATCHES=no`) | nothing |
| `hooks` | with the patches | nothing |
| `audit` | with the patches | `pg_io_audit` |
| `crypt` | with the patches | `pg_page_crypt`, cluster converted |

Each configuration has its own cluster with the same `pgbench` data. The data
is much larger than `shared_buffers` (scale 100, about 1.5 GB, against
128 MB), so pages move between shared buffers and the relation files all the
time, through the storage manager and its hooks. The files themselves mostly
stay in the page cache of the operating system, so the measurement shows the
CPU cost of the hooks rather than disk speed.

A round runs the configurations in turn (`vanilla`, `hooks`, `audit`,
`crypt`), each with a read-only test (`pgbench -S`, primary key lookups) and
the default read-write test (TPC-B-like), 60 seconds each. The overall speed
of a machine drifts between rounds, which affects all configurations of a
round alike, so each configuration is compared with `vanilla` of the same
round; the tables show the median and the range of these differences over 5
rounds.

Before each test the script times a fixed CPU-bound loop and stores the time
with the result. If it varies by more than 30%, the summary warns that the
results are not reliable. This matters on laptops: a first attempt on a
laptop (Intel i5-1235U under WSL2) slowed down about tenfold in the middle of
the run because of thermal throttling, which made that run useless. The
numbers here therefore come from GitHub-hosted runners, where the calibration
varied by 2% and 6%.

## Results

PostgreSQL 19.0, scale 100, `shared_buffers = 128MB`, 4 clients, 5 rounds of
60 seconds, `io_method = worker` (the default). Workflow
`.github/workflows/bench.yml` on `ubuntu-24.04` runners (4 CPUs, 16 GB).

### Run 1

CPU calibration 759 to 774 ms. (This run did not record the CPU model yet.)

| Configuration | Workload | Median tps | vs. vanilla (median) | vs. vanilla (range) |
|---------------|----------|-----------:|---------------------:|--------------------:|
| vanilla | read-only | 112034 | | |
| hooks | read-only | 115151 | +1.3% | −0.9% .. +2.8% |
| audit | read-only | 112701 | +0.6% | −3.9% .. +1.6% |
| crypt | read-only | 101341 | −11.5% | −12.3% .. −5.6% |
| vanilla | read-write | 10663 | | |
| hooks | read-write | 10426 | −1.4% | −4.6% .. +2.5% |
| audit | read-write | 10198 | −3.0% | −4.9% .. −0.2% |
| crypt | read-write | 9789 | −6.5% | −9.5% .. −4.7% |

### Run 2

Intel Xeon Platinum 8573C, 4 CPUs, 15.6 GB, CPU calibration 996 to 1053 ms.
This runner was slower, in particular on the read-write test (less than half
the throughput of run 1), which points to slower storage.

| Configuration | Workload | Median tps | vs. vanilla (median) | vs. vanilla (range) |
|---------------|----------|-----------:|---------------------:|--------------------:|
| vanilla | read-only | 84147 | | |
| hooks | read-only | 84582 | +0.2% | −0.6% .. +1.5% |
| audit | read-only | 83125 | −0.0% | −2.7% .. +0.8% |
| crypt | read-only | 75008 | −9.0% | −13.3% .. −8.5% |
| vanilla | read-write | 4542 | | |
| hooks | read-write | 4557 | +0.2% | −1.0% .. +1.3% |
| audit | read-write | 4471 | −0.2% | −2.5% .. +1.1% |
| crypt | read-write | 4407 | −1.5% | −3.9% .. −0.9% |

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
