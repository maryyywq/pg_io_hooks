# pg_io_hooks

[![CI](https://github.com/maryyywq/pg_io_hooks/actions/workflows/ci.yml/badge.svg)](https://github.com/maryyywq/pg_io_hooks/actions/workflows/ci.yml)

Storage I/O transformation hooks for PostgreSQL 17, and prototype extensions
built on top of them.

PostgreSQL has a rich system of hooks and extensions, but no way for an
extension to look at, or change, the blocks that move between shared buffers
and the relation files. Transparent encryption of data files, compression,
integrity checks or I/O auditing can therefore only be done by forking the
server. This project adds a small set of hooks to the storage manager and
proves with two extensions that they are enough.

## What is in the repository

| Part | Where | Description |
|------|-------|-------------|
| Patches for PostgreSQL 17 | [`patches/`](patches/) | 8 hooks in the storage manager (157 lines in `smgr.h` and `smgr.c`) and a test module |
| `pg_page_crypt` | [`extensions/pg_page_crypt/`](extensions/pg_page_crypt/) | Transparent encryption of relation files (AES-256-XTS), with a tool that converts a cluster |
| `pg_io_audit` | [`extensions/pg_io_audit/`](extensions/pg_io_audit/) | Counts and traces storage I/O operations; shows them in SQL views |
| Tests | [`tests/`](tests/), `t/` of each extension, the test module in the patches | 345 checks, all run by one script and by CI |
| Scripts | [`scripts/`](scripts/) | Build PostgreSQL with the patches, run all tests |
| Documentation | [`docs/`](docs/) | API reference, design rationale, tests, limitations |

## How it works

```
 backends, background writer, checkpointer, startup process (WAL replay)
                               |
                        buffer manager
          (sets the page checksum before a write, verifies it after a read)
                               |
          +--------------------+---------------------+
          |                  smgr.c                  |   <- the hooks
          |   read  write  extend  zeroextend  sync  |
          |   truncate  create  unlink               |
          +--------------------+---------------------+
                               |
                      md.c (segment files)
```

An extension sets the hook variables in `_PG_init()`, as with any PostgreSQL
hook:

- **Transforming hooks** (`smgr_read_hook`, `smgr_write_hook`,
  `smgr_extend_hook`) can change the block data on its way to and from the
  file. `pg_page_crypt` uses them to encrypt and decrypt.
- **Notification hooks** (`smgr_zeroextend_hook`, `smgr_sync_hook`,
  `smgr_truncate_hook`, `smgr_create_hook`, `smgr_unlink_hook`) report what
  happens to a relation file. `pg_io_audit` uses all of the hooks to count
  operations.

With no hook set the server behaves as before; each hook costs one test of a
NULL pointer. See [docs/hooks-api.md](docs/hooks-api.md) for the contract and
[docs/design.md](docs/design.md) for why it is built this way.

## Quick start

You need a Debian or Ubuntu system (or the equivalent packages), about 1 GB of
disk space and, on a multi-core machine, roughly ten to fifteen minutes in
total.

```
git clone https://github.com/maryyywq/pg_io_hooks.git
cd pg_io_hooks

scripts/install-deps-debian.sh   # build tools and libraries (uses sudo)
scripts/build-postgres.sh        # fetch PostgreSQL 17, apply the patches, build, install
scripts/run-tests.sh             # run everything
```

`build-postgres.sh` puts everything under `./build` (see the comments at the
top of the script for the variables that change this, including a `release`
build). When it finishes it prints the `PATH` to use.

### Using the extensions

Install them into that server (the tests do this too):

```
export PATH="$PWD/build/install/bin:$PATH"
make -C extensions/pg_io_audit install
make -C extensions/pg_page_crypt install
```

Then follow the README of each extension:

- [`pg_io_audit`](extensions/pg_io_audit/README.md): add it to
  `shared_preload_libraries`, run `CREATE EXTENSION pg_io_audit`, and query
  `pg_io_audit_stats`.
- [`pg_page_crypt`](extensions/pg_page_crypt/README.md): create a key,
  stop the cluster, convert it with `pg_page_crypt_convert`, then start it
  with the extension loaded.

## Status

The hooks and both extensions are implemented and tested (see
[docs/testing.md](docs/testing.md)). This is a research prototype; its
[limitations](docs/limitations.md) matter, in particular:

- `pg_page_crypt` does not encrypt the WAL or the system catalogs, offers no
  authentication beyond page checksums, and keeps the key in a file. It is not
  meant for production.
- Performance has not been measured yet.

## Documentation

- [Hooks API reference](docs/hooks-api.md): the hooks, their contract and an
  example extension
- [Design and rationale](docs/design.md): the choices and how they compare
  with other proposals
- [Testing](docs/testing.md): what is tested and how to run it
- [Limitations](docs/limitations.md)
- [Patches](patches/README.md): the patch series and how to apply it

## License

[PostgreSQL License](LICENSE).
