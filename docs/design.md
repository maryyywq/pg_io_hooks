# Design and rationale

This document explains why the hooks look the way they do. For the
reference of the API see [hooks-api.md](hooks-api.md); for what the design
does not cover see [limitations.md](limitations.md).

## The problem

PostgreSQL has a rich system of hooks and extensions, but nothing in the
storage layer lets an extension look at, or change, the blocks that move
between shared buffers and the relation files. Features that need this
(transparent encryption of data files, compression, extra integrity checks,
I/O auditing, redirecting pages to another store) can only be built today by
forking the server.

This project adds a small, general set of hooks to the storage manager and
shows with two extensions that they are enough:

- `pg_page_crypt` encrypts relation files (a transforming extension),
- `pg_io_audit` counts and traces I/O operations (an observing extension).

## Goals and constraints

- **Minimal, localized patch.** Only the storage manager changes: 157 added
  lines in `smgr.h` and `smgr.c` (most of them comments).
- **No behavior change by default.** With no hook set, the server does the
  same as before. The cost is a test of a NULL pointer per call.
- **No change of on-disk formats.** The page layout, the WAL format and
  `pg_control` are untouched. A cluster works with or without the patch as
  long as no extension changes the data.
- **Enabled for the whole cluster**, through `shared_preload_libraries`.
  Selecting relations or tablespaces individually is not attempted.
- **Block size does not change.** A transformation must produce a block of
  the same size, because the addressing of blocks and WAL records depends on
  it.

## Where to put the hooks

The storage stack is: access methods and buffer manager, then
`smgr.c` (a dispatch layer with a table of storage managers, which today has
one entry), then `md.c` (the magnetic disk storage manager that reads and
writes segment files), then the file layer.

| Option | Where | For | Against |
|--------|-------|-----|---------|
| A | the functions of `smgr.c` | one place; sees every user of the storage manager; `md.c` untouched | the hook interface has to deal with vectored calls and `const` buffers |
| B | inside `md.c`, around the file reads and writes | closest to the real I/O | several functions to change, with logic duplicated; I/O paths of `md.c` change |
| C | in `bufmgr.c` and `localbuf.c` | the buffer descriptor is at hand | misses bulk writes, relation copying and other direct users of `smgr` |
| D | replace the table of storage managers by a plug-in mechanism chosen per relation or tablespace | the most general | a large change; needs a way to select the manager during recovery; far beyond what the hooks need |

**Option A was chosen.** Every part of the server that touches relation files
through the storage manager is covered by changing `smgr.c` only, including
the buffer manager (shared and local), bulk writes (index builds, rewrites),
relation copying and WAL replay. Because the hooks sit above `md.c`, the code
of the file layer, segmenting, fsync handling and so on is not touched.

An important property of this placement is the order of the operations around
the hooks. The buffer manager sets the page checksum before it calls
`smgrwritev()`, and verifies it after `smgrreadv()` returns. The hooks run
inside those calls, so the write hook sees a page whose checksum is already
set, and the read hook has finished before the checksum is verified. Nothing
else has to know about the transformation.

## Asynchronous reads (PostgreSQL 18 and later)

PostgreSQL 18 added asynchronous I/O. The buffer manager no longer calls
`smgrreadv()`: it starts a read with `smgrstartreadv()` and gets the result
later, through completion callbacks registered on the I/O handle. The
checksum check moved into such a callback. A hook in `smgrreadv()` alone would
therefore never see most reads, and the server would verify checksums of
pages that were never decrypted. This happens with every `io_method`,
including `sync`, which only executes the same callbacks right away.

So for these versions the patch adds a completion callback of the storage
manager, `PGAIO_HCB_SMGR_READV`, which `smgrstartreadv()` registers when a
read hook is set and which calls the hook. Callbacks run in the reverse order
of registration, which puts it between the callback of `md.c` and that of the
buffer manager: after the result was turned into a number of blocks, and
before the checksums are verified. The property described above still holds,
and `md.c` is still not changed.

Other options were rejected:

- calling the hook from `md_readv_complete()` would put it below the switch
  of storage managers, in `md.c`;
- forcing synchronous reads would not help (see above) and would give up the
  point of asynchronous I/O;
- a callback registered by the extension itself is not possible: the table
  of callbacks is fixed in the core (callbacks are referred to by a small
  number so that they work across processes).

Completion callbacks run in a critical section and may run in another
process than the one that started the read: an I/O worker, or with
`io_uring` any backend that collects the completion. That changes the
contract of the read hook: it gets the `RelFileLocatorBackend` instead of an
`SMgrRelation` (which need not exist in that process), and it cannot raise
an error, so it returns the number of blocks it processed and the callback
turns a failure into an I/O error with its own message. The same API is used
on PostgreSQL 17, where the hook is simply called at the end of
`smgrreadv()`, so that an extension needs no version-specific code.

Writes, extension, truncation and the other operations are still synchronous
in PostgreSQL 18 and 19, and their hooks did not change.

## What the hooks cover and what they do not

Covered: everything that goes through `smgr.c`. In PostgreSQL 17 to 19 that
is the read and write paths of shared and local buffers, appending blocks, zero
extension, syncing, truncation, creation and removal of relation files.

Not covered, because they do not use the storage manager:

- the WAL, SLRU files (`pg_xact` and others), temporary files for sorting and
  similar, `pg_control`, configuration;
- tools and server code that read or copy relation files directly:
  `pg_basebackup`, `pg_checksums`, `pg_rewind`, `pg_combinebackup`, and, in
  the server, `copydir.c` (`CREATE DATABASE ... STRATEGY = FILE_COPY`,
  `ALTER DATABASE ... SET TABLESPACE`), `reinit.c` (resetting unlogged
  relations after a crash) and the removal of whole directories by
  `DROP DATABASE` and `DROP TABLESPACE`.

Anything that transforms data must keep these direct users in mind. How
`pg_page_crypt` does this is described below.

## Checksums are the core's job

A transformation could in principle be responsible for page checksums too:
compute them over the transformed page, as an integrity check of the stored
form. That was rejected for these reasons.

- The hook would have to know how to compute and verify checksums, and to do
  it in both directions, which every extension would repeat.
- With the core computing the checksum over the ordinary page and verifying it
  after the read hook, a wrong key or a corrupted block shows up as a checksum
  failure on the decrypted data: it is an end-to-end check, with no change in
  the core's checksum code.
- It keeps hooks simple: a transformation is just a function from a block to
  a block.

The price: tools that read files directly and verify checksums (such as
`pg_checksums`) cannot verify a transformed file, because the checksum covers
the page as it is in memory. For encryption, the page header that holds the
checksum is encrypted as well.

## One transformer, any number of observers

Hooks are installed the usual PostgreSQL way: each extension saves the
previous hook and calls it. For observers this is enough. Two extensions that
both transform data are a different matter: a read has to undo the changes
in the opposite order from the one a write used, and neither hook knows
about the other. The patches therefore define a rule instead of trying to
build a general pipeline: **only one extension changes data; all others pass
it through.** Load order then does not matter, which `tests/hook_chain`
verifies with `pg_io_audit` before and after `pg_page_crypt`.

## The write hook never changes its input

The buffer given to a write can be the live shared buffer, which other
processes may be reading. A transforming hook therefore writes its result
into a buffer of its own and returns a pointer to it. The extensions use
static, direct-I/O-aligned buffers, so nothing is allocated at write time
(allocation in a hook would be a problem in places like critical sections).
The read hook, in contrast, changes the destination buffer in place, as the
buffer was just filled for the caller.

## Zero blocks

New pages are created by extending the file with zeroes
(`smgrzeroextend()`), without calling a write or extend hook, and an
all-zero page means "unused" to PostgreSQL. A transformation that turned zero
into something else on the way in would break this on the way out, so zero
blocks are never transformed.

## What was deliberately left out

- **No marker in the page header.** Another proposal for similar hooks (see
  below) reserved bits of `pd_flags` to tag transformed pages. That uses a
  resource the core needs, and it does not help with the files that existed
  before the transformation was enabled, which still have to be converted.
  This design uses a state file in the data directory and an offline
  conversion tool instead (see `extensions/pg_page_crypt`).
- **No WAL hooks.** Data files and the WAL are different problems (the WAL
  is written in order and read for replay and by external tools). The WAL
  stays plaintext, which is stated as a limitation of `pg_page_crypt`.
- **No per-relation or per-tablespace selection**, and no changes of block
  size. Both would be extensions of this work.

## How `pg_page_crypt` handles the direct users

The extension encrypts blocks of relations with file numbers of 16384 and
above. The cipher is AES-256-XTS. Its tweak is the file number and block
number, deliberately without the fork, database or tablespace:

- the server copies the init fork of an unlogged relation over its main
  fork with plain file operations (`reinit.c`), so both forks must decrypt
  the same way;
- `CREATE DATABASE ... STRATEGY = FILE_COPY` copies files without the storage
  manager into a different database, so the database must not be part of the
  tweak.

The same choice lets `pg_rewind` and `pg_upgrade` work unchanged: both copy
relation files or blocks as they are, and keep the file and block numbers.

Encryption needs the data checksum as its only integrity check (XTS does not
authenticate), so the extension refuses to start without data checksums.
A cluster is encrypted once, while stopped, by a conversion tool, which
leaves a state file with the fingerprint of the key; the server refuses to
start when the file is missing or the key does not match.

## Related work

- **"Extensible storage manager API - smgr hooks"** (pgsql-hackers, 2021,
  and follow-ups). The goal is more general: let a plug-in provide a whole
  storage manager, so the table of managers in `smgr.c` becomes dynamic. The
  discussion raised questions such as how a manager is chosen per tablespace
  or relation and how redo works. It is not part of PostgreSQL (as of 19).
- **"RFC: PostgreSQL Storage I/O Transformation Hooks"** (pgsql-hackers,
  December 2025). Closest to this project. It adds hooks in `md.c` for read,
  write and extend, and in the WAL insertion and decoding paths, plus a
  transform ID in `pd_flags`. In the discussion, reviewers raised concerns
  about hook chaining, about using page header bits, and about the cost of
  keeping the hooks stable compared with an extensible storage manager.
- **`pg_tde`** (Percona) implements encryption with a patched PostgreSQL.

This project differs in where the hooks are (the `smgr.c` interface, so all
users of the storage manager are covered without touching `md.c`), in the
notification hooks for syncing, truncating, creating and removing files, in
its stated contract for chaining (one transformer, any number of observers),
and in not using page header bits or WAL hooks.

Links:

- <https://postgresql.org/message-id/CAP4vRV6JKXyFfEOf=n+v5RGsZywAQ3CTM8ESWvgq+S87Tmgx_g@mail.gmail.com>
- <https://www.postgresql.org/message-id/flat/CAAAe_zCHNVKxETAWBZg3YWPdmXsf-G2OcQtQOYQn4AeZaES4tQ%40mail.gmail.com>
- <https://www.percona.com/blog/your-datas-not-safe-until-its-tde-safe-heres-how/>
