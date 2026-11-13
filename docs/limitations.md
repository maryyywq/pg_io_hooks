# Limitations

This is a research prototype. The limitations below are known and, unless
noted, deliberate.

## The hooks

- **PostgreSQL 17 only.** The patches apply to the base commit named in
  [patches/README.md](../patches/README.md). They were not ported to other
  versions.
- **Only what goes through the storage manager.** The WAL, SLRU files,
  temporary files and `pg_control` are not seen by the hooks. Tools and
  server code that read or copy relation files directly (`pg_basebackup`,
  `pg_checksums`, `pg_rewind`, `pg_combinebackup`, `CREATE DATABASE ...
  STRATEGY = FILE_COPY`, resetting unlogged relations after a crash) bypass
  them.
- **One transforming extension.** Several extensions can install hooks, but
  only one may change data; the others must pass it through
  ([design.md](design.md#one-transformer-any-number-of-observers)). Nothing
  in the server enforces this.
- **Size-preserving transformations only.** A transformed block must have the
  size of the original.
- **Cluster-wide.** No selection per relation or tablespace.
- **No WAL hooks.**
- **Performance has not been measured** (see [testing.md](testing.md)).

## `pg_page_crypt`

- **System catalogs are not encrypted.** Only relation files with a file
  number of 16384 or more are (user relations and TOAST, indexes, sequences,
  materialized views). The files of the catalogs created by `initdb` are not,
  until a command such as `VACUUM FULL` gives them a new file.
- **The WAL is not encrypted.** Full-page images and records contain
  plaintext, as do SLRU files, temporary files and server logs.
- **No authentication.** XTS does not detect tampering by itself. Corruption
  is noticed only through the page checksum (16 bits), so data checksums are
  required. This is a defense against reading the files, not against changing
  them.
- **Equal data at the same position gives equal ciphertext** in the forks of
  one relation (the XTS tweak is the file number and block number, without the
  fork). This is needed to keep the unlogged-relation reset working.
- **Conversion is offline and not crash safe.** The cluster must be shut down
  cleanly; the tool works in place, so take a backup first. An interrupted
  conversion leaves a marker file, and the server refuses to start.
- **Key management is minimal:** one key in a file, no rotation, no key
  hierarchy. A standby or restored copy needs the same key and the same
  configuration.

### Behavior of PostgreSQL tools with an encrypted cluster

| Tool | Result |
|------|--------|
| `pg_basebackup` | works; the copy needs the same module and key to start. It cannot verify encrypted pages (their headers are encrypted) and does not report errors for them |
| `pg_checksums --check` | reports every encrypted block as a bad checksum; do not use |
| `pg_rewind`, `pg_upgrade`, `pg_combinebackup` | not tested |

## `pg_io_audit`

- Counters live in shared memory and start from zero at each server start.
- Counts are requests seen by the hooks, taken before the file system
  operation, which may still fail.
- `unlink` counts relations, not forks: the hook is called for every fork
  number whether or not the fork exists.
- The trace is best effort. With many concurrent writers and a small ring, an
  entry can be overwritten while it is being read and is then skipped.
- Only relation files are seen, not the WAL, SLRU or temporary files.
