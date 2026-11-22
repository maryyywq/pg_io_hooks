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
| `tests/hook_chain`, `t/001_chain.pl` | 22 | `pg_io_audit` and `pg_page_crypt` together, in both load orders |

In total, per run of `scripts/run-tests.sh`:

| PostgreSQL | Regression suite | Other checks | Total |
|------------|-----------------:|-------------:|------:|
| 17 | 229 | 119 | 348 |
| 18 | 235 | 3 × 128 (`sync`, `worker`, `io_uring`) | 619 |
| 19 | 243 | 3 × 128 (`sync`, `worker`, `io_uring`) | 627 |

These are the numbers from runs of the scripts from scratch (fetching
PostgreSQL, applying the patches), with debug builds (assertions enabled).

## Where each test lives

- The test module and its tests are part of the patches
  (`src/test/modules/test_smgr_hooks`, patches `0002`, `0004` and `0006`). It uses
  XOR instead of a cipher, so a failure points at the hooks, not at crypto.
- The extensions' tests are in `extensions/<name>/t/` and run with
  `make installcheck` against an installed server.
- Tests of extensions together are in `tests/`.

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
- **Direct tools.** Running `pg_basebackup` against an encrypted cluster
  works (checked by the replication test). `pg_checksums` reports all
  encrypted blocks as bad; this was checked by hand and is documented in
  [limitations.md](limitations.md), not asserted in a test.

## Not covered yet

- Performance. The design claim is that unset hooks cost one NULL test per
  call; this has not been measured, and neither has the overhead of the
  extensions.
- `pg_rewind`, `pg_upgrade`, `pg_combinebackup` and logical replication with
  an encrypted cluster.
- Servers built without OpenSSL, other platforms, other PostgreSQL versions
  (including other minor releases of 18 and 19 than the base of the patches).
