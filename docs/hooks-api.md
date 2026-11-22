# Hooks API reference

The patches add eight hooks to the PostgreSQL storage manager (`smgr`).
They are declared in `src/include/storage/smgr.h` and called from
`src/backend/storage/smgr/smgr.c`. This page describes what each hook does,
when it is called, and the rules an extension must follow. The reasons for
the design are in [design.md](design.md).

The API is the same for PostgreSQL 17, 18 and 19, so an extension builds for
all three from one source. Where the versions differ in how a hook is
called, this page says so.

## Overview

Hooks come in two kinds.

**Transforming hooks** can change the data that is read from or written to a
relation file. They are what makes features such as transparent encryption
possible.

| Hook | Called | May change |
|------|--------|------------|
| `smgr_read_hook` | after blocks were read from a relation file | the blocks just read, in place |
| `smgr_write_hook` | before blocks are written to a relation file | which buffers are written |
| `smgr_extend_hook` | before one block is appended to a relation file | which buffer is written |

**Notification hooks** only tell the extension that something happened. They
return nothing and cannot change the operation.

| Hook | Called |
|------|--------|
| `smgr_zeroextend_hook` | before zero blocks are appended to a relation file |
| `smgr_sync_hook` | before a relation file is synced to disk, or registered for the next checkpoint |
| `smgr_truncate_hook` | before a relation file is truncated |
| `smgr_create_hook` | after the storage of a fork was created |
| `smgr_unlink_hook` | before the files of a fork are removed |

Every hook variable is `NULL` by default. When no extension sets a hook, the
server behaves exactly as it does without the patches; the only cost is one
test of a pointer per call.

## Common rules

- **Loading.** Hooks are called from every process that does relation I/O:
  backends, the background writer, the checkpointer, the startup process
  (WAL replay) and, for the read hook on PostgreSQL 18 and later, I/O
  workers. An extension that changes the on-disk format must therefore
  be loaded with `shared_preload_libraries`, so that it is active in all of
  them from the start. Check `process_shared_preload_libraries_in_progress`
  in `_PG_init()`.
- **Registration and chaining.** Register as with any PostgreSQL hook: save
  the previous value of the variable in `_PG_init()`, install your function,
  and call the previous hook from yours. See [Chaining](#chaining).
- **Errors.** Report problems with `ereport(ERROR, ...)`. The operation then
  fails the same way a failing file read or write does. The exceptions are
  the read hook, which reports a failure through its return value, and the
  truncate and unlink hooks, described below.
- **Checksums.** Page checksums are not the hooks' concern. The core computes
  a page's checksum before the write hook runs and verifies it after the read
  hook has run, so both hooks always work with the ordinary (untransformed)
  page.
- **Context.** Hooks receive the `SMgrRelation`, which identifies the
  relation file (`reln->smgr_rlocator.locator` is the tablespace, database
  and relation file number; `reln->smgr_rlocator.backend` tells whether it is
  a temporary relation), the fork, and the block number. The read and unlink
  hooks receive the `RelFileLocatorBackend` (the same as
  `reln->smgr_rlocator`) instead. Block numbers are numbers within the
  relation, not within a 1 GB segment file.

## Transforming hooks

### `smgr_read_hook`

```c
typedef BlockNumber (*smgr_read_hook_type) (RelFileLocatorBackend rlocator,
                                            ForkNumber forknum,
                                            BlockNumber blocknum,
                                            void **buffers,
                                            BlockNumber nblocks);
```

Called after `nblocks` blocks starting at `blocknum` were read into
`buffers`, and before anyone looks at them. The hook may change the blocks in
place (for example decrypt them).

Because the page checksums are verified after this hook, they are checked
against the decrypted pages.

**Return value.** The hook returns the number of leading blocks it processed
successfully, normally `nblocks`. A smaller value `n` makes the read fail
with

```
ERROR:  could not transform block <blocknum + n> in file "<path>" after reading it
```

The buffers are not marked valid, so a later read of the same blocks reads
them from the file again and calls the hook again. A hook that chains to a
previous one processes only as many blocks as the previous hook returned,
and returns at most that number.

**Where it runs.** On PostgreSQL 17 the hook is called at the end of
`smgrreadv()`, in the process that reads. From PostgreSQL 18 on, the buffer
manager reads with asynchronous I/O (`smgrstartreadv()`), and the hook is
called from the completion callback of the read, which:

- runs in a **critical section**, so the hook **must not raise an error or
  allocate memory** (`palloc`). Logging with `elog(LOG)` is allowed;
- may run in **another process** than the one that started the read,
  depending on `io_method`: the process itself (`sync`), an I/O worker
  (`worker`), or whichever backend collects the completion (`io_uring`). So
  the hook must use only its arguments, state that `_PG_init()` set up (which
  every process inherits from the postmaster), and shared memory. This is
  also why it gets a `RelFileLocatorBackend` and not an `SMgrRelation`: the
  relation need not be open in that process;
- only sees the blocks that were actually read. After a short read the
  caller starts another read for the rest, and the hook is called again for
  those blocks.

Synchronous `smgrreadv()` calls (used for example when copying a relation)
still call the hook directly. The rules are the same on PostgreSQL 17, so an
extension that follows them works on all versions.

Blocks that were read past the end of the file during recovery are zero-filled
by the server and also reach the hook; a transformation must leave all-zero
blocks alone (see [Zero blocks](#zero-blocks)).

### `smgr_write_hook`

```c
typedef const void **(*smgr_write_hook_type) (SMgrRelation reln,
                                              ForkNumber forknum,
                                              BlockNumber blocknum,
                                              const void **buffers,
                                              BlockNumber nblocks);
```

Called at the start of `smgrwritev()`. It returns the array of buffers that
must be written instead of `buffers`; return the argument unchanged if the
hook does not transform the data.

The hook **must not modify the buffers it is given.** A buffer passed to a
write can be the live shared buffer (this is the case when data checksums are
disabled), which other processes may be reading at that moment. Encrypt into a buffer of your own
and return that.

Rules for the returned memory:

- The returned array, and the buffers it points to, belong to the hook.
- The buffers must be aligned to `PG_IO_ALIGN_SIZE` (needed for direct I/O).
- They must stay valid until the next call of a write hook in the same
  process.
- The hook must be prepared for up to `MAX_IO_COMBINE_LIMIT` blocks per call.
- Allocate the memory in advance (for example static arrays or memory
  allocated in `_PG_init()`), not inside the hook.

### `smgr_extend_hook`

```c
typedef const void *(*smgr_extend_hook_type) (SMgrRelation reln,
                                              ForkNumber forknum,
                                              BlockNumber blocknum,
                                              const void *buffer);
```

Called at the start of `smgrextend()`, which appends one block to a relation
file (for example while building an index). It is the single-block counterpart
of the write hook, with the same rules for the returned buffer.

## Notification hooks

```c
typedef void (*smgr_zeroextend_hook_type) (SMgrRelation reln, ForkNumber forknum,
                                           BlockNumber blocknum, int nblocks);
typedef void (*smgr_sync_hook_type) (SMgrRelation reln, ForkNumber forknum,
                                     bool immediate);
typedef void (*smgr_truncate_hook_type) (SMgrRelation reln, ForkNumber forknum,
                                         BlockNumber old_nblocks,
                                         BlockNumber nblocks);
typedef void (*smgr_create_hook_type) (SMgrRelation reln, ForkNumber forknum,
                                       bool isRedo);
typedef void (*smgr_unlink_hook_type) (RelFileLocatorBackend rlocator,
                                       ForkNumber forknum, bool isRedo);
```

- **`smgr_zeroextend_hook`** is called before `smgrzeroextend()` adds `nblocks`
  zero blocks starting at `blocknum`. This is how a relation normally grows;
  the new blocks are zeroes and do not pass through the extend or write hooks.
- **`smgr_sync_hook`** is called before a fork is synced. `immediate` is true
  for a sync that happens now (`smgrimmedsync()`, and `smgrdosyncall()`) and
  false for one queued for the next checkpoint (`smgrregistersync()`).
- **`smgr_truncate_hook`** is called before a fork is truncated from
  `old_nblocks` to `nblocks` blocks. `smgrtruncate2()` (`smgrtruncate()` from
  PostgreSQL 18 on) normally runs inside a
  critical section, so this hook **must not allocate memory or raise an
  error.**
- **`smgr_create_hook`** is called after the storage of a fork was created.
  `isRedo` is true during WAL replay.
- **`smgr_unlink_hook`** is called before the files of a fork are removed,
  once for **every** fork number, whether or not that fork exists. Unlinking
  happens after the outcome of the transaction is decided, so this hook
  **must not raise an error**; use `WARNING`. Only the relation identifier is
  available, because the `SMgrRelation` has already been closed.

## Where the hooks are called

| Hook | Place in `smgr.c` | Position |
|------|-------------------|----------|
| `smgr_read_hook` | `smgrreadv()`; from PostgreSQL 18 on also the AIO completion callback `PGAIO_HCB_SMGR_READV` of `smgrstartreadv()` | after the read |
| `smgr_write_hook` | `smgrwritev()` | before the write |
| `smgr_extend_hook` | `smgrextend()` | before the write |
| `smgr_zeroextend_hook` | `smgrzeroextend()` | before |
| `smgr_sync_hook` | `smgrimmedsync()`, `smgrregistersync()`, `smgrdosyncall()` | before |
| `smgr_truncate_hook` | `smgrtruncate2()` (17), `smgrtruncate()` (18, 19), once per fork | before |
| `smgr_create_hook` | `smgrcreate()` | after |
| `smgr_unlink_hook` | `smgrdounlinkall()`, once per fork | before |

`smgrread()` and `smgrwrite()` are inline wrappers for the vectored functions
`smgrreadv()` and `smgrwritev()` with one block, so they are covered too. All
users of the storage manager are covered: the buffer manager, bulk writes
(`bulk_write.c`), relation copying, WAL replay.

On PostgreSQL 18 and later, `smgrstartreadv()` registers the
`PGAIO_HCB_SMGR_READV` callback on the I/O handle when a read hook is set.
Completion callbacks run in the reverse order of registration, so it runs
after the callback of `md.c` (which turns the result into a number of blocks
and reports I/O errors) and before the one of the buffer manager (which
verifies the page checksums):

```
bufmgr: register SHARED_BUFFER_READV -> smgrstartreadv: register SMGR_READV
    -> mdstartreadv: register MD_READV, start the read
completion: MD_READV -> SMGR_READV (calls smgr_read_hook) -> SHARED_BUFFER_READV
```

Some commands create or remove relation files as whole directories, without
the storage manager, and therefore without the create and unlink hooks:

| Command | Files created without `smgr_create_hook` | Files removed without `smgr_unlink_hook` |
|---------|------------------------------------------|------------------------------------------|
| `CREATE DATABASE ... STRATEGY = FILE_COPY` | all files of the new database | |
| `ALTER DATABASE ... SET TABLESPACE` | all files in the new location | all files in the old location |
| `DROP DATABASE` | | all files of the database |
| `DROP TABLESPACE` | | the (empty) tablespace directory |

Data in files copied this way also does not pass through the read and write
hooks: the bytes are copied as they are on disk.

## Chaining

Extensions install hooks the usual PostgreSQL way: save the previous value in
`_PG_init()` and call it from your own function.

```c
static smgr_write_hook_type prev_write_hook = NULL;

static const void **
my_write_hook(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
              const void **buffers, BlockNumber nblocks)
{
    if (prev_write_hook)
        buffers = prev_write_hook(reln, forknum, blocknum, buffers, nblocks);

    /* ... observe, or transform and return your own buffers ... */
    return buffers;
}

void
_PG_init(void)
{
    prev_write_hook = smgr_write_hook;
    smgr_write_hook = my_write_hook;
}
```

The rule that makes this safe: **only one extension may actually change data.**
Every other extension must pass the data through, that is, call the previous
hook and return what it returned. Then any number of observers can sit on
either side of the one transformer, in any load order. Transformations
cannot be composed in general: on a read they would have to be undone in the
reverse of the order they were applied in a write, and the hooks have no way
to agree on that.

The extensions in this repository follow the rule: `pg_page_crypt` is the
transformer and `pg_io_audit` is a pass-through observer. The test in
`tests/hook_chain` checks both load orders.

## Zero blocks

Relation extension writes zero-filled blocks without calling the write or
extend hook (it uses `smgrzeroextend()`), and PostgreSQL treats an all-zero
page as an empty, unused page. A transformation that stores a non-zero
image for a block would make such pages unreadable. The extensions here
therefore never transform a block that consists only of zeroes, in either
direction.

## Writing an extension

A minimal observer that counts the blocks written:

```c
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/smgr.h"

PG_MODULE_MAGIC;

void _PG_init(void);

static smgr_write_hook_type prev_write_hook = NULL;
static uint64 blocks_written = 0;

static const void **
count_write_hook(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
                 const void **buffers, BlockNumber nblocks)
{
    if (prev_write_hook)
        buffers = prev_write_hook(reln, forknum, blocknum, buffers, nblocks);
    blocks_written += nblocks;
    return buffers;
}

void
_PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
        ereport(ERROR,
                (errmsg("must be loaded via shared_preload_libraries")));
    prev_write_hook = smgr_write_hook;
    smgr_write_hook = count_write_hook;
}
```

(The counter is per process. Hooks run in many processes, so a real
extension keeps its data in shared memory; `extensions/pg_io_audit` shows how.)

For a complete example that transforms data, see
`extensions/pg_page_crypt`; the test module
`src/test/modules/test_smgr_hooks` in the patches is a smaller one.
