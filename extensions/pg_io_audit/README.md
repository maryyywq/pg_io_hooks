# pg_io_audit

Audit of storage I/O operations in PostgreSQL, built on the smgr I/O
transformation hooks from `patches/`.

The extension counts every operation that passes through the storage manager
and can keep a trace of the most recent ones. It only observes: it calls the
previous hook and returns its result, so it can be loaded together with an
extension that transforms data (for example page encryption).

## Requirements

PostgreSQL 17, 18 or 19 with the matching patch series from
`../../patches/` applied.

## Build and install

```
make PG_CONFIG=/path/to/pg_config
make PG_CONFIG=/path/to/pg_config install
```

## Configuration

The library must be loaded at server start:

```
shared_preload_libraries = 'pg_io_audit'
```

| Parameter                 | Default | Context    | Description                                           |
|---------------------------|---------|------------|-------------------------------------------------------|
| `pg_io_audit.enabled`     | `on`    | `sighup`   | Counts operations (and traces them if enabled)        |
| `pg_io_audit.trace_size`  | `0`     | `postmaster` | Number of recent operations kept in a ring buffer; `0` disables the trace |

Then, in each database where the views are needed:

```
CREATE EXTENSION pg_io_audit;
```

## Usage

`pg_io_audit_stats` has one row per operation and fork seen since the server
started or since the last reset:

| Column   | Description                                                        |
|----------|--------------------------------------------------------------------|
| `op`     | `read`, `write`, `extend`, `zeroextend`, `immedsync`, `registersync`, `truncate`, `create`, `unlink` |
| `fork`   | `main`, `fsm`, `vm`, `init`                                        |
| `calls`  | Number of calls                                                    |
| `blocks` | Number of blocks involved (for `truncate`: blocks removed)         |

`pg_io_audit_trace` (needs `pg_io_audit.trace_size > 0`) lists the recent
operations, oldest first: `seq`, `ts`, `pid`, `op`, `fork`, `tablespace`,
`database`, `relfilenode`, `block`, `nblocks`.

`SELECT pg_io_audit_reset();` zeroes the counters and empties the trace
(superuser only). The views are readable by superusers and members of
`pg_monitor`.

## Example

With `pg_io_audit.trace_size = 1000`, on PostgreSQL 19. Create a table, fill
it and write it out with a checkpoint:

```
SELECT pg_io_audit_reset();
CREATE TABLE accounts (id int PRIMARY KEY, balance numeric);
INSERT INTO accounts SELECT g, 100 FROM generate_series(1, 100000) g;
CHECKPOINT;

SELECT * FROM pg_io_audit_stats ORDER BY op, fork;
      op      | fork | calls | blocks
--------------+------+-------+--------
 create       | fsm  |     1 |      0
 create       | main |     4 |      0
 extend       | main |     2 |      2
 read         | fsm  |     6 |      6
 read         | main |    50 |     50
 read         | vm   |     2 |      2
 registersync | main |     2 |      0
 write        | fsm  |     3 |      3
 write        | main |   866 |    866
 write        | vm   |    10 |     10
 zeroextend   | fsm  |     1 |      3
 zeroextend   | main |   816 |    816
```

The table and its index grow by about 800 blocks (`zeroextend`), which the
checkpoint writes out (`write`); `create` counts the files of the table, its
TOAST table and their indexes.

After a restart, with empty shared buffers, the trace shows how one row is
found by its primary key: the metapage, the root and a leaf page of the
index (`accounts_pkey`, file 16402), then a page of the table (file 16396):

```
SELECT pg_io_audit_reset();
SELECT * FROM accounts WHERE id = 42;

SELECT op, fork, relfilenode, block, nblocks FROM pg_io_audit_trace
WHERE relfilenode IN (16396, 16402) AND op = 'read' AND fork = 'main'
ORDER BY seq;
  op  | fork | relfilenode | block | nblocks
------+------+-------------+-------+---------
 read | main |       16402 |     0 |       1
 read | main |       16402 |     3 |       1
 read | main |       16402 |     1 |       1
 read | main |       16396 |     0 |       1
```

## Notes and limitations

- Counters live in shared memory and start from zero at every server start,
  and after the server reinitializes itself following a crash of a server
  process.
- Counts are requests seen by the hooks, taken after the previous hook has
  run; an operation that later fails in the file system is still counted.
- `unlink` counts relations (it is recorded for the main fork only), because
  the hook is invoked for every fork number whether or not the fork exists.
- `create` and `unlink` do not count files of whole databases that are
  copied or removed at once: `CREATE DATABASE ... STRATEGY = FILE_COPY`,
  `ALTER DATABASE ... SET TABLESPACE`, `DROP DATABASE`. These commands work
  on directories without the storage manager.
- The trace is best effort. Entries that are being written when the trace is
  read are skipped. If the ring is much smaller than the number of concurrent
  writers, an entry can, rarely, contain fields of two events.
- Only operations on relation files are seen. WAL, SLRU files and temporary
  files do not go through the storage manager.

## Tests

```
make installcheck
```

runs the TAP test against the installed server and extension.
