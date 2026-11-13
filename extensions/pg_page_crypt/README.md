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

- PostgreSQL 17 with the patch series from `../../patches/` applied
- OpenSSL development files
- Data checksums enabled in the cluster (`initdb --data-checksums`)

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
  indexes, sequences, TOAST tables, materialized views.
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
- **Equal data at the same position gives equal ciphertext** in the forks of
  one relation, because the XTS tweak is built from the file number and the
  block number but not the fork. This is needed because the server copies the
  init fork over the main fork of unlogged relations as plain files.
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

runs a TAP test against the installed server and module: conversion in both
directions, reading and writing, crash recovery, unlogged relations,
`VACUUM FULL`, `CLUSTER`, `CREATE DATABASE`, wrong key and missing conversion.
