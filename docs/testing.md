# Testing

## Running the tests

With a PostgreSQL 17 that has the patches applied (see the main
[README](../README.md#quick-start)):

```
scripts/run-tests.sh
```

The script runs every step below and prints a summary; its exit status is
non-zero if any step failed. `SKIP_CORE=1` skips the PostgreSQL regression
suite, which takes a few minutes. The same script runs in the CI workflow
(`.github/workflows/ci.yml`) on every push.

Most tests are TAP tests (Perl, `PostgreSQL::Test::Cluster`): each starts its
own temporary servers, so they do not touch any running server.

## What is tested

| Step | Checks | What it shows |
|------|-------:|---------------|
| PostgreSQL regression suite (`make check`) | 229 | the patched server, with no hook set, passes the standard tests |
| `test_smgr_hooks`, `t/001_transform.pl` | 10 | read, write and extend hooks: data is stored transformed, read back correctly, survives a restart and crash recovery, and is rejected when the module is not loaded |
| `test_smgr_hooks`, `t/002_events.pl` | 6 | the notification hooks (create, zeroextend, sync, truncate, unlink) are called |
| `pg_io_audit`, `t/001_audit.pl` | 15 | counters, trace, reset and privileges |
| `pg_page_crypt`, `t/001_crypt.pl` | 32 | conversion in both directions, reading and writing, crash recovery, unlogged relations, `VACUUM FULL`, `CLUSTER`, `REINDEX`, `CREATE DATABASE`, wrong key, missing conversion, refusing clusters without checksums |
| `pg_page_crypt`, `t/002_replication.pl` | 12 | base backup, standby from the backup, WAL replay through the hooks, standby crash, promotion, start without a key |
| `tests/hook_chain`, `t/001_chain.pl` | 22 | `pg_io_audit` and `pg_page_crypt` together, in both load orders |

In total 326 checks. These are the numbers from a run on a fresh clone of the
repository, with a debug build (assertions enabled).

## Where each test lives

- The test module and its tests are part of the patches
  (`src/test/modules/test_smgr_hooks`, patches `0002` and `0004`). It uses
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
- Servers built without OpenSSL, other platforms, other PostgreSQL versions.
