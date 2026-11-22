# Limitations

This is a research prototype. The limitations below are known and, unless
noted, deliberate.

## The hooks

- **PostgreSQL 17, 18 and 19 only.** The patches for each version apply to
  the base commit named in [patches/README.md](../patches/README.md) (a
  `REL_17_12`, `REL_18_7` and `REL_19_0`); other minor releases
  may need a rebase.
- **The read hook cannot raise errors and runs in other processes.** On
  PostgreSQL 18 and later it runs in an I/O completion callback (critical
  section, possibly an I/O worker or another backend); see
  [hooks-api.md](hooks-api.md#smgr_read_hook). A failure fails the whole
  read; the hook cannot say why, the error only names the block.
- **Only what goes through the storage manager.** The WAL, SLRU files,
  temporary files and `pg_control` are not seen by the hooks. Tools and
  server code that read or copy relation files directly (`pg_basebackup`,
  `pg_checksums`, `pg_rewind`, `pg_combinebackup`, `CREATE DATABASE ...
  STRATEGY = FILE_COPY`, `ALTER DATABASE ... SET TABLESPACE`, resetting
  unlogged relations after a crash) bypass them.
- **Whole-database file operations are not reported.** `DROP DATABASE`,
  `DROP TABLESPACE`, `ALTER DATABASE ... SET TABLESPACE` and `CREATE DATABASE
  ... STRATEGY = FILE_COPY` create or remove files without calling the create
  and unlink hooks (see the table in [hooks-api.md](hooks-api.md#where-the-hooks-are-called)).
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
  until a command such as `VACUUM FULL` gives them a new file, which is then
  encrypted like any other (the conversion tool handles such files in
  `base/` and in `global/`).
- **The WAL is not encrypted.** Full-page images and records contain
  plaintext, as do SLRU files, temporary files and server logs.
- **No authentication.** XTS does not detect tampering by itself. Corruption
  is noticed only through the page checksum (16 bits), so data checksums are
  required. This is a defense against reading the files, not against changing
  them.
- **The encryption is deterministic**, as is usual for disk encryption with
  XTS: the same data in the same place always gives the same ciphertext.
  Consequences:
  - Someone who has two copies of an encrypted file from different moments
    (for example two backups) can see which 16-byte parts of a block changed
    between them, though not what they contain.
  - The place is the relation file number and the block number only, not the
    fork, the database or the tablespace. Equal blocks at the same position
    in the forks of one relation, or in relations of different databases
    that happen to have the same file number, give equal ciphertext. Leaving
    those out is necessary: the server copies files between forks (the reset
    of unlogged relations) and between databases and tablespaces
    (`CREATE DATABASE ... STRATEGY = FILE_COPY`,
    `ALTER DATABASE ... SET TABLESPACE`) without decrypting them.
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

- Counters live in shared memory and start from zero at each server start,
  and again when the server reinitializes after a crash of a server process.
- Counts are requests seen by the hooks, taken before the file system
  operation, which may still fail. The exception are reads: the read hook
  runs after the read, so they count the blocks actually read. On
  PostgreSQL 18 and later it runs when an asynchronous read completes, so the
  `pid` in the trace is that of the process that completed the read (for
  example an I/O worker), not necessarily the one that asked for the data.
- `unlink` counts relations, not forks: the hook is called for every fork
  number whether or not the fork exists.
- `create` and `unlink` do not count the files of whole databases that are
  copied or removed at once (`CREATE DATABASE ... STRATEGY = FILE_COPY`,
  `ALTER DATABASE ... SET TABLESPACE`, `DROP DATABASE`).
- The trace is best effort. Entries that are being written when the trace is
  read are skipped. In the rare case that the ring wraps around while a slow
  writer is still filling a slot (a ring much smaller than the number of
  concurrent writers), an entry can contain fields of two events.
- Only relation files are seen, not the WAL, SLRU or temporary files.
