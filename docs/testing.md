# Testing

## Running the tests

With a PostgreSQL that has the patches applied (see the main
[README](../README.md#quick-start)):

```
scripts/run-tests.sh                  # PostgreSQL 19
PG_VERSION=18 scripts/run-tests.sh    # or 18, or 17
```

The script runs every step below and prints a summary; its exit status is
non-zero if any step failed. `SKIP_CORE=1` skips the PostgreSQL regression
suite, which takes a few minutes. The same script runs in the CI workflow
(`.github/workflows/ci.yml`) on every push, for each of the three versions.

On PostgreSQL 18 and 19 reads complete asynchronously, in a way that depends
on `io_method`, so every step except the regression suite runs once for each
of `sync`, `worker` and `io_uring` (the last one only if the server was built
with liburing; `IO_METHODS` selects the values). The script writes the
setting to a file named by `TEMP_CONFIG`, which `PostgreSQL::Test::Cluster`
adds to the configuration of every server a test creates.

Most tests are TAP tests (Perl, `PostgreSQL::Test::Cluster`): each starts its
own temporary servers, so they do not touch any running server.

## Upgrade test

```
PG_VERSION=17 scripts/build-postgres.sh   # or 18
PG_VERSION=19 scripts/build-postgres.sh
OLD_VERSION=17 scripts/run-upgrade-test.sh
```

tests `pg_upgrade` of a cluster encrypted with `pg_page_crypt` from 17 (or
18) to 19: the data, a large object and its catalog rewritten by
`VACUUM FULL`, indexes and a materialized view survive, `amcheck` finds no
problems, and the new cluster stores the data encrypted. It needs two
versions, so it has its own script; CI runs it in a separate job for 17 → 19
and 18 → 19.

## Code style

```
scripts/check-style.sh                  # PostgreSQL 19
PG_VERSION=17 scripts/check-style.sh    # or 18
```

checks that the core files changed by the patches are formatted with
`pgindent` of that PostgreSQL version, that the C sources of the extensions
are formatted with `pgindent` of PostgreSQL 19 (their own type names are in
`extensions/typedefs.list`; versions of `pg_bsd_indent` differ in details, so
one version is used for them), and runs `shellcheck` on the scripts. CI runs
it for every version.

## What is tested

| Step | Checks | What it shows |
|------|-------:|---------------|
| PostgreSQL regression suite (`make check`) | 229 / 235 / 243 | the patched server, with no hook set, passes the standard tests (17 / 18 / 19) |
| `test_smgr_hooks`, `t/001_transform.pl` | 10 | read, write and extend hooks: data is stored transformed, read back correctly, survives a restart and crash recovery, and is rejected when the module is not loaded |
| `test_smgr_hooks`, `t/002_events.pl` | 6 | the notification hooks (create, zeroextend, sync, truncate, unlink) are called |
| `test_smgr_hooks`, `t/003_read_errors.pl` | 3 / 12 | a failure reported by the read hook fails the read with an error, and the data is read again once the hook succeeds; on 18 and 19 for each `io_method` (17 / 18 and 19) |
| `pg_io_audit`, `t/001_audit.pl` | 15 | counters, trace, reset and privileges |
| `pg_page_crypt`, `t/001_crypt.pl` | 32 | conversion in both directions, reading and writing, crash recovery, unlogged relations, `VACUUM FULL`, `CLUSTER`, `REINDEX`, `CREATE DATABASE`, wrong key, missing conversion, refusing clusters without checksums |
| `pg_page_crypt`, `t/002_replication.pl` | 12 | base backup, standby from the backup, WAL replay through the hooks, standby crash, promotion, start without a key |
| `pg_page_crypt`, `t/003_tool.pl` | 19 | the conversion tool with a shared catalog rewritten before the conversion, with tablespaces (and another server version's directory in the same location, which must stay untouched), with a data directory path that contains a quote; `ALTER TABLE` / `ALTER DATABASE ... SET TABLESPACE` on encrypted data |
| `pg_page_crypt`, `t/004_rewind.pl` | 22 | `pg_rewind` of a failed-over primary that has changes of its own, from the data directory and from a running server of the new primary; the rewound server has the data of the new primary, stores it encrypted and continues as its standby |
| `pg_page_crypt`, `t/005_crash_under_load.pl` | 15 | four crashes (`kill -9` of a backend, immediate shutdown) during a `pgbench` write workload with data much larger than shared buffers; after each, the `pgbench` balances agree and `amcheck` finds no problems |
| `tests/hook_chain`, `t/001_chain.pl` | 22 | `pg_io_audit` and `pg_page_crypt` together, in both load orders |

In total, per run of `scripts/run-tests.sh`:

| PostgreSQL | Regression suite | Other checks | Total |
|------------|-----------------:|-------------:|------:|
| 17 | 229 | 156 | 385 |
| 18 | 235 | 3 × 165 (`sync`, `worker`, `io_uring`) | 730 |
| 19 | 243 | 3 × 165 (`sync`, `worker`, `io_uring`) | 738 |

The upgrade test runs separately (see below) and has 14 checks for each
pair of versions.

These are the numbers from runs of the scripts from scratch (fetching
PostgreSQL, applying the patches), with debug builds (assertions enabled).

## Where each test lives

- The test module and its tests are part of the patches
  (`src/test/modules/test_smgr_hooks`, patches `0002`, `0004` and `0006`). It uses
  XOR instead of a cipher, so a failure points at the hooks, not at crypto.
- The extensions' tests are in `extensions/<name>/t/` and run with
  `make installcheck` against an installed server.
- Tests of extensions together, and the upgrade test, are in `tests/`.

## Notes on specific tests

- **Encrypted at rest.** The `pg_page_crypt` tests read the relation files
  directly and check that a known string in the table does not appear in
  them, and that it does appear in the same file of a cluster without the
  extension, so the check cannot pass by accident.
- **Wrong key and missing conversion.** The server must refuse to start; the
  tests check the error messages in the server log.
- **Unlogged relations.** After a crash the server copies the init fork over
  the main fork as plain files. The test crashes a server with an unlogged
  table to cover that path.
- **Consistency after crashes.** `scripts/build-postgres.sh` also installs
  `amcheck`, which the crash, rewind and upgrade tests use to check the
  structure of heap and B-tree pages, in addition to comparing data.
- **Direct tools.** Running `pg_basebackup` against an encrypted cluster
  works (checked by the replication test). `pg_checksums` reports all
  encrypted blocks as bad; this was checked by hand and is documented in
  [limitations.md](limitations.md), not asserted in a test.

## Not covered yet

- Performance is measured separately, not by these tests; see
  [performance.md](performance.md) for what is and is not covered.
- `pg_combinebackup` and logical replication with an encrypted cluster.
- `pg_upgrade` with `--link`, `--clone` or `--swap`; only the default copy
  mode is tested.
- Servers built without OpenSSL, other platforms, other PostgreSQL versions
  (including other minor releases of 18 and 19 than the base of the patches).
