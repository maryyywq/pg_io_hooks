# Patches for PostgreSQL 17, 18 and 19

Series of `git format-patch` files that add the smgr I/O transformation
hooks to the PostgreSQL core, one directory per major version. Each series
applies, in order, to a fixed commit, also recorded in its `0001-*.patch` as
`base-commit:`:

| Directory | Base | Commit |
|-----------|------|--------|
| `17/` | a commit of `REL_17_STABLE` | `179f22f256ab50bdd0a4351a8b9def13876e4c43` |
| `18/` | tag `REL_18_7` (18.7) | `f5c41f6681c910b3f69951bf6fb97c6d74605d25` |
| `19/` | tag `REL_19_0` (19.0) | `5c839a2a08b9b87a349be97371fac6c5a2873b10` |

The three series have the same patches and the same hooks API:

| Patch | Description |
|-------|-------------|
| `0001` | `smgr_read_hook`, `smgr_write_hook`, `smgr_extend_hook`: hooks that can transform block data (`smgr.h`, `smgr.c`; on 18 and 19 also the AIO completion callback `PGAIO_HCB_SMGR_READV` in `aio.h` and `aio_callback.c`) |
| `0002` | `src/test/modules/test_smgr_hooks`: test module that XORs blocks, with a TAP test |
| `0003` | `smgr_zeroextend_hook`, `smgr_sync_hook`, `smgr_truncate_hook`, `smgr_create_hook`, `smgr_unlink_hook`: notification hooks |
| `0004` | Test of the notification hooks in `test_smgr_hooks` |
| `0005` | The new types in `src/tools/pgindent/typedefs.list` |
| `0006` | Test of read hook failures in `test_smgr_hooks` (for every `io_method` on 18 and 19) |

The differences between the versions are in where the read hook is called.
PostgreSQL 18 introduced asynchronous I/O, and the buffer manager reads with
`smgrstartreadv()` instead of `smgrreadv()`; on 18 and 19 the read hook is
therefore also called from an I/O completion callback. See
[docs/design.md](../docs/design.md#asynchronous-reads-postgresql-18-and-later).

The core change consists only of added lines: 185 on PostgreSQL 17
(`smgr.h` 129, mostly the comments that define the contract of the hooks,
and `smgr.c` 56) and 310 on 18 and 19 (`smgr.h` 134, `smgr.c` 172, of which
about 100 are the completion callback, and 2 lines each in `aio.h` and
`aio_callback.c`); the rest is the test module. With no hook set, each hook
costs one test of a NULL pointer.

## Applying

```
git clone https://github.com/postgres/postgres.git
cd postgres
git checkout REL_19_0          # or REL_18_7, or 179f22f256ab50bdd0a4351a8b9def13876e4c43 for 17
git am /path/to/pg_io_hooks/patches/19/*.patch
```

`scripts/build-postgres.sh` does this for the version in `PG_VERSION`, then
builds and installs the server; see the main [README](../README.md). The
hooks are documented in [docs/hooks-api.md](../docs/hooks-api.md).

## Regenerating

After changing the hooks branch of a PostgreSQL checkout for version `N`
with base commit `BASE`:

```
git format-patch --base=BASE -o /path/to/pg_io_hooks/patches/N BASE..HEAD
```
