# pg_page_crypt

Transparent encryption of relation files in PostgreSQL, built on the smgr I/O
transformation hooks from `patches/`.

Blocks of user relations are encrypted with AES-256-XTS when they are written
and decrypted when they are read, so data files on disk (including copies in
backups) are unreadable without the key. The server, applications and
queries work as usual.

This is a prototype. Key management is deliberately simple and the module is
not meant for production use.

## Requirements

- PostgreSQL 17, 18 or 19 with the matching patch series from
  `../../patches/` applied
- OpenSSL development files
- Data checksums enabled in the cluster (the default of `initdb` since
  PostgreSQL 18; use `initdb --data-checksums` on 17)

## Build and install

```
make PG_CONFIG=/path/to/pg_config
make PG_CONFIG=/path/to/pg_config install
```

This installs the module `pg_page_crypt` and the tool
`pg_page_crypt_convert`.

## Setup

1. Create a key: 128 hexadecimal characters (two different 256-bit AES
   keys) in a file that is not accessible to group or others:

   ```
   umask 077
   openssl rand -hex 64 > /path/to/pg_page_crypt.key
   ```

2. Stop the cluster cleanly and convert it. Take a backup first: the
   conversion is done in place and is not crash safe.

   ```
   pg_page_crypt_convert --encrypt -D $PGDATA --key-file /path/to/pg_page_crypt.key
   ```

3. Configure the server and start it:

   ```
   shared_preload_libraries = 'pg_page_crypt'
   pg_page_crypt.key_file = '/path/to/pg_page_crypt.key'
   ```

The server refuses to start if the cluster has not been converted, if the key
does not match the one used for conversion, if data checksums are disabled,
or if an earlier conversion did not finish.

To go back, stop the cluster cleanly, remove the module from
`shared_preload_libraries` and run
`pg_page_crypt_convert --decrypt -D $PGDATA --key-file ...`.

The tool needs `pg_controldata` in `PATH`, and refuses to run if the server
is running or was not shut down cleanly.

## What is encrypted

- All forks of relations with a file number of 16384 or more: user tables,
  indexes, sequences, TOAST tables, materialized views, and system catalogs
  that a command such as `VACUUM FULL` has given a new file (shared catalogs
  in `global/` included).
- In all tablespaces. The conversion tool processes only this server
  version's subdirectory of a tablespace location and leaves those of other
  clusters alone.
- Blocks that consist only of zeroes are never transformed (that is how new,
  unused pages look on disk).

## Limitations

- **System catalogs are not encrypted.** Their files have file numbers below
  16384 (until a command such as `VACUUM FULL` rewrites them).
- **WAL is not encrypted.** Full-page images and records contain plaintext.
  Neither are SLRU files (`pg_xact` and others), temporary files and
  server logs.
- **No authentication.** XTS does not detect tampering by itself; corruption is
  noticed only through page checksums, a 16-bit value, which is why data
  checksums are required.
- **The encryption is deterministic** (as usual for XTS disk encryption):
  comparing two copies of a file from different moments shows which 16-byte
  parts of a block changed. The position used by the cipher is the file
  number and block number only, without the fork, database or tablespace,
  so equal blocks at the same position in the forks of one relation, or in
  relations of different databases with the same file number, give equal
  ciphertext. This is required because the server copies such files without
  decrypting them (resetting unlogged relations, `CREATE DATABASE ... STRATEGY
  = FILE_COPY`, `ALTER DATABASE ... SET TABLESPACE`). See
  [docs/limitations.md](../../docs/limitations.md).
- **Tools that read relation files directly** see ciphertext, because the
  page header is encrypted too:
  - `pg_checksums` reports every encrypted block as a checksum failure and
    cannot be used.
  - `pg_basebackup` works and its copy contains the state file, so a standby
    or restored copy needs the same module and key. It does not report
    errors for encrypted pages, but it cannot verify them either.
  - `pg_rewind` was not tested.
- **Key handling is minimal:** a key file, no rotation. A standby needs the
  same key and the same configuration.

## Tests

```
make installcheck
```

runs the TAP tests against the installed server and module:

- `t/001_crypt.pl`: conversion in both directions, reading and writing,
  crash recovery, unlogged relations, `VACUUM FULL`, `CLUSTER`,
  `CREATE DATABASE`, wrong key and missing conversion;
- `t/002_replication.pl`: base backup, standby, promotion;
- `t/003_tool.pl`: the conversion tool with tablespaces, with shared catalogs
  rewritten before the conversion, with a data directory path that contains
  a quote, and moving encrypted data between tablespaces.
