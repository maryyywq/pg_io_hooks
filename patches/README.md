# Patches for PostgreSQL 17

A series of `git format-patch` files that add the smgr I/O transformation
hooks to the PostgreSQL 17 core. They apply, in order, to this commit of the
`REL_17_STABLE` branch (also recorded in `0001-*.patch` as `base-commit:`):

```
179f22f256ab50bdd0a4351a8b9def13876e4c43
```

| Patch | Description |
|-------|-------------|
| `0001` | `smgr_read_hook`, `smgr_write_hook`, `smgr_extend_hook`: hooks that can transform block data (`smgr.h`, `smgr.c`) |
| `0002` | `src/test/modules/test_smgr_hooks`: test module that XORs blocks, with a TAP test |
| `0003` | `smgr_zeroextend_hook`, `smgr_sync_hook`, `smgr_truncate_hook`, `smgr_create_hook`, `smgr_unlink_hook`: notification hooks |
| `0004` | Test of the notification hooks in `test_smgr_hooks` |
| `0005` | The new types in `src/tools/pgindent/typedefs.list` |

The core change is 157 added lines in two files: `src/include/storage/smgr.h`
(112 lines, mostly the comments that define the contract of the hooks) and
`src/backend/storage/smgr/smgr.c` (45 lines); the rest is the test module.
With no hook set, each hook costs one test of a NULL pointer.

## Applying

```
git clone https://github.com/postgres/postgres.git
cd postgres
git checkout 179f22f256ab50bdd0a4351a8b9def13876e4c43
git am /path/to/pg_io_hooks/patches/*.patch
```

`scripts/build-postgres.sh` does this, then builds and installs the server;
see the main [README](../README.md). The hooks are documented in
[docs/hooks-api.md](../docs/hooks-api.md).

## Regenerating

After changing the `smgr-hooks` branch of a PostgreSQL checkout:

```
git format-patch --base=179f22f256ab50bdd0a4351a8b9def13876e4c43 \
    -o /path/to/pg_io_hooks/patches 179f22f256ab50bdd0a4351a8b9def13876e4c43..HEAD
```
