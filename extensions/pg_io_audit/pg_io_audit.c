/*-------------------------------------------------------------------------
 *
 * pg_io_audit.c
 *		Audit of storage I/O operations through the smgr hooks.
 *
 * What it does
 * ------------
 * The extension counts every operation that goes through the storage manager
 * (read, write, extend, zeroextend, sync, truncate, create, unlink), per
 * operation type and fork, and can optionally keep a trace of the most
 * recent operations in a ring buffer.  The data is shown by two views,
 * pg_io_audit_stats and pg_io_audit_trace (see the SQL script).
 *
 * Why shared memory: the hooks are called from many processes (backends, the
 * background writer, the checkpointer, the startup process), and the numbers
 * must add up across all of them.  So everything lives in one shared memory
 * structure, AuditState.  Counters are atomic integers, which makes the hot
 * path (a hook call) lock-free: it costs a few atomic additions.
 *
 * Hooks only observe.  They call the previous hook first and return what it
 * returned, so the extension can be combined with one module that actually
 * transforms data (for example pg_page_crypt); see smgr.h for the rules.
 *
 * The extension must be loaded with shared_preload_libraries, because shared
 * memory can only be reserved while the server starts.
 *
 * IDENTIFICATION
 *		extensions/pg_io_audit/pg_io_audit.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/relpath.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

/* SQL-callable functions, see pg_io_audit--1.0.sql */
PG_FUNCTION_INFO_V1(pg_io_audit_get_stats);
PG_FUNCTION_INFO_V1(pg_io_audit_get_trace);
PG_FUNCTION_INFO_V1(pg_io_audit_reset);

void		_PG_init(void);

/* The operations we count, one per kind of smgr hook call */
typedef enum AuditOp
{
	AUDIT_READ,
	AUDIT_WRITE,
	AUDIT_EXTEND,
	AUDIT_ZEROEXTEND,
	AUDIT_SYNC_IMMEDIATE,
	AUDIT_SYNC_REGISTERED,
	AUDIT_TRUNCATE,
	AUDIT_CREATE,
	AUDIT_UNLINK,
	AUDIT_NUM_OPS
} AuditOp;

/* Names shown in the views, in the same order as AuditOp */
static const char *const audit_op_names[AUDIT_NUM_OPS] = {
	"read", "write", "extend", "zeroextend", "immedsync", "registersync",
	"truncate", "create", "unlink"
};

/*
 * Counters of one operation on one fork.  Both are updated with atomic
 * additions and read without any lock, so a reader may see the two values
 * from slightly different moments; that is fine for statistics.
 */
typedef struct AuditCounter
{
	pg_atomic_uint64 calls;		/* number of calls */
	pg_atomic_uint64 blocks;	/* number of blocks involved */
} AuditCounter;

/*
 * One slot of the trace ring buffer.
 *
 * Writers do not take a lock.  To let readers detect a slot that is being
 * written (or was overwritten while they copied it), "seq" works like a
 * sequence lock: it is zero while the slot is being filled and idx + 1 once
 * it holds event number idx.  A reader accepts a slot only if "seq" has the
 * expected value both before and after copying the fields.
 */
typedef struct AuditTraceEntry
{
	pg_atomic_uint64 seq;
	TimestampTz ts;
	int			pid;
	uint8		op;				/* an AuditOp */
	uint8		fork;			/* a ForkNumber */
	Oid			spcOid;			/* the three parts of the RelFileLocator */
	Oid			dbOid;
	Oid			relNumber;
	BlockNumber block;			/* first block involved */
	uint32		nblocks;		/* number of blocks involved */
} AuditTraceEntry;

/*
 * The shared memory structure: the counters, and the trace ring buffer,
 * which has audit_trace_size slots (zero if tracing is off).
 */
typedef struct AuditState
{
	pg_atomic_uint64 trace_head;	/* number of events traced so far */
	AuditCounter counters[AUDIT_NUM_OPS][MAX_FORKNUM + 1];
	AuditTraceEntry trace[FLEXIBLE_ARRAY_MEMBER];
} AuditState;

/* Points to the shared structure once the server has started */
static AuditState *audit_state = NULL;

/* GUCs */
static bool audit_enabled = true;
static int	audit_trace_size = 0;

/* Previous hook values, to chain to them */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static smgr_read_hook_type prev_read_hook = NULL;
static smgr_write_hook_type prev_write_hook = NULL;
static smgr_extend_hook_type prev_extend_hook = NULL;
static smgr_zeroextend_hook_type prev_zeroextend_hook = NULL;
static smgr_sync_hook_type prev_sync_hook = NULL;
static smgr_truncate_hook_type prev_truncate_hook = NULL;
static smgr_create_hook_type prev_create_hook = NULL;
static smgr_unlink_hook_type prev_unlink_hook = NULL;

/* Size of the shared structure for the configured trace size */
static Size
audit_memsize(void)
{
	return add_size(offsetof(AuditState, trace),
					mul_size(audit_trace_size, sizeof(AuditTraceEntry)));
}

/*
 * shmem_request_hook: tell the server how much shared memory we need.  This
 * must be done before the server allocates its shared memory.
 */
static void
audit_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(audit_memsize());
}

/* Zero all counters and empty the trace. */
static void
audit_reset_state(AuditState *state)
{
	for (int op = 0; op < AUDIT_NUM_OPS; op++)
	{
		for (int fork = 0; fork <= MAX_FORKNUM; fork++)
		{
			pg_atomic_write_u64(&state->counters[op][fork].calls, 0);
			pg_atomic_write_u64(&state->counters[op][fork].blocks, 0);
		}
	}
	pg_atomic_write_u64(&state->trace_head, 0);
	for (int i = 0; i < audit_trace_size; i++)
		pg_atomic_write_u64(&state->trace[i].seq, 0);
}

/*
 * shmem_startup_hook: create (or attach to) the shared structure.  The
 * postmaster runs this when it creates shared memory: at startup, and again
 * after a crash of a server process, when shared memory is created anew (so
 * the counters start from zero then too).  "found" is true only when the
 * structure already exists, which happens in builds where every process
 * attaches to shared memory by itself (EXEC_BACKEND, as on Windows); then
 * the structure must be left as it is.
 */
static void
audit_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	audit_state = ShmemInitStruct("pg_io_audit", audit_memsize(), &found);
	if (!found)
	{
		for (int op = 0; op < AUDIT_NUM_OPS; op++)
		{
			for (int fork = 0; fork <= MAX_FORKNUM; fork++)
			{
				pg_atomic_init_u64(&audit_state->counters[op][fork].calls, 0);
				pg_atomic_init_u64(&audit_state->counters[op][fork].blocks, 0);
			}
		}
		pg_atomic_init_u64(&audit_state->trace_head, 0);
		for (int i = 0; i < audit_trace_size; i++)
			pg_atomic_init_u64(&audit_state->trace[i].seq, 0);
	}
}

/*
 * Record one operation: bump the counters and, if tracing is on, append an
 * entry to the ring buffer.
 *
 * Tracing is best effort.  Each event takes a slot number from the atomic
 * counter trace_head, so concurrent writers get different slots, until the
 * ring wraps around.  A reader skips a slot that is being written.  If the
 * ring is so small that a slow writer is still filling a slot when another
 * writer, one full round later, takes the same slot, the two can interleave
 * and a reader may then see fields of both events.  A ring that is large
 * compared with the number of concurrent writers makes this practically
 * impossible.
 */
static void
audit_event(AuditOp op, ForkNumber fork, RelFileLocator rlocator,
			BlockNumber block, uint32 nblocks)
{
	AuditState *state = audit_state;

	/*
	 * No shared state yet (a process that runs before shared memory is set
	 * up) or counting switched off.
	 */
	if (state == NULL || !audit_enabled)
		return;

	pg_atomic_fetch_add_u64(&state->counters[op][fork].calls, 1);
	pg_atomic_fetch_add_u64(&state->counters[op][fork].blocks, nblocks);

	if (audit_trace_size > 0)
	{
		uint64		idx = pg_atomic_fetch_add_u64(&state->trace_head, 1);
		AuditTraceEntry *e = &state->trace[idx % audit_trace_size];

		/* Mark the slot as being written, then fill it, then publish it. */
		pg_atomic_write_u64(&e->seq, 0);
		pg_write_barrier();
		e->ts = GetCurrentTimestamp();
		e->pid = MyProcPid;
		e->op = op;
		e->fork = fork;
		e->spcOid = rlocator.spcOid;
		e->dbOid = rlocator.dbOid;
		e->relNumber = rlocator.relNumber;
		e->block = block;
		e->nblocks = nblocks;
		pg_write_barrier();
		pg_atomic_write_u64(&e->seq, idx + 1);
	}
}

/*
 * The hooks below follow one pattern: call the previous hook, record the
 * operation, and for hooks that return something, return what the previous
 * hook returned.  Recording happens after the previous hook, so the
 * counters reflect requests that passed through the whole chain, but they
 * are counted before the actual file system operation, which may still
 * fail.
 */

static void
audit_read_hook(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				void **buffers, BlockNumber nblocks)
{
	if (prev_read_hook)
		prev_read_hook(reln, forknum, blocknum, buffers, nblocks);

	audit_event(AUDIT_READ, forknum, reln->smgr_rlocator.locator,
				blocknum, nblocks);
}

static const void **
audit_write_hook(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				 const void **buffers, BlockNumber nblocks)
{
	if (prev_write_hook)
		buffers = prev_write_hook(reln, forknum, blocknum, buffers, nblocks);

	audit_event(AUDIT_WRITE, forknum, reln->smgr_rlocator.locator,
				blocknum, nblocks);

	/* Never change the buffers: we only observe. */
	return buffers;
}

static const void *
audit_extend_hook(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				  const void *buffer)
{
	if (prev_extend_hook)
		buffer = prev_extend_hook(reln, forknum, blocknum, buffer);

	audit_event(AUDIT_EXTEND, forknum, reln->smgr_rlocator.locator,
				blocknum, 1);
	return buffer;
}

static void
audit_zeroextend_hook(SMgrRelation reln, ForkNumber forknum,
					  BlockNumber blocknum, int nblocks)
{
	if (prev_zeroextend_hook)
		prev_zeroextend_hook(reln, forknum, blocknum, nblocks);

	audit_event(AUDIT_ZEROEXTEND, forknum, reln->smgr_rlocator.locator,
				blocknum, nblocks);
}

/*
 * "immediate" distinguishes a sync that happens now (smgrimmedsync) from one
 * that is queued for the next checkpoint (smgrregistersync).  No block
 * numbers are involved.
 */
static void
audit_sync_hook(SMgrRelation reln, ForkNumber forknum, bool immediate)
{
	if (prev_sync_hook)
		prev_sync_hook(reln, forknum, immediate);

	audit_event(immediate ? AUDIT_SYNC_IMMEDIATE : AUDIT_SYNC_REGISTERED,
				forknum, reln->smgr_rlocator.locator, 0, 0);
}

/*
 * Truncation normally runs inside a critical section, where memory must not
 * be allocated and errors are not allowed.  audit_event() only does atomic
 * operations on preallocated shared memory, so it is safe here.
 */
static void
audit_truncate_hook(SMgrRelation reln, ForkNumber forknum,
					BlockNumber old_nblocks, BlockNumber nblocks)
{
	if (prev_truncate_hook)
		prev_truncate_hook(reln, forknum, old_nblocks, nblocks);

	/*
	 * For a truncate, "block" is the new size and "nblocks" the number of
	 * blocks removed.
	 */
	audit_event(AUDIT_TRUNCATE, forknum, reln->smgr_rlocator.locator,
				nblocks, old_nblocks > nblocks ? old_nblocks - nblocks : 0);
}

static void
audit_create_hook(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	if (prev_create_hook)
		prev_create_hook(reln, forknum, isRedo);

	audit_event(AUDIT_CREATE, forknum, reln->smgr_rlocator.locator, 0, 0);
}

static void
audit_unlink_hook(RelFileLocatorBackend rlocator, ForkNumber forknum,
				  bool isRedo)
{
	if (prev_unlink_hook)
		prev_unlink_hook(rlocator, forknum, isRedo);

	/*
	 * The hook is called for every fork number whether or not the fork
	 * exists, but every relation has a main fork: count relations, not forks.
	 */
	if (forknum == MAIN_FORKNUM)
		audit_event(AUDIT_UNLINK, forknum, rlocator.locator, 0, 0);
}

/*
 * Module initialization.  It runs in the postmaster while the server starts.
 */
void
_PG_init(void)
{
	/* Shared memory can only be reserved at server start. */
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errmsg("pg_io_audit must be loaded via shared_preload_libraries")));

	/*
	 * Counting can be switched off with a configuration reload, without a
	 * restart (the shared memory stays allocated).
	 */
	DefineCustomBoolVariable("pg_io_audit.enabled",
							 "Enables counting of storage I/O operations.",
							 NULL,
							 &audit_enabled,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	/*
	 * The size of the trace decides how much shared memory is reserved, so it
	 * can only be changed with a restart.
	 */
	DefineCustomIntVariable("pg_io_audit.trace_size",
							"Number of recent I/O operations kept in the trace.",
							"Zero disables the trace.",
							&audit_trace_size,
							0,
							0,
							10000000,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	MarkGUCPrefixReserved("pg_io_audit");

	/* Reserve and initialize shared memory. */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = audit_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = audit_shmem_startup;

	/* Install the smgr hooks, remembering the previous ones. */
	prev_read_hook = smgr_read_hook;
	smgr_read_hook = audit_read_hook;
	prev_write_hook = smgr_write_hook;
	smgr_write_hook = audit_write_hook;
	prev_extend_hook = smgr_extend_hook;
	smgr_extend_hook = audit_extend_hook;
	prev_zeroextend_hook = smgr_zeroextend_hook;
	smgr_zeroextend_hook = audit_zeroextend_hook;
	prev_sync_hook = smgr_sync_hook;
	smgr_sync_hook = audit_sync_hook;
	prev_truncate_hook = smgr_truncate_hook;
	smgr_truncate_hook = audit_truncate_hook;
	prev_create_hook = smgr_create_hook;
	smgr_create_hook = audit_create_hook;
	prev_unlink_hook = smgr_unlink_hook;
	smgr_unlink_hook = audit_unlink_hook;
}

/*
 * The shared state, or an error if the extension is not loaded through
 * shared_preload_libraries (the SQL objects exist but nothing collects data).
 */
static AuditState *
audit_get_state(void)
{
	if (audit_state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_io_audit must be loaded via shared_preload_libraries")));
	return audit_state;
}

/*
 * pg_io_audit_get_stats() -- one row per operation and fork that has been
 * seen at least once.  Backs the view pg_io_audit_stats.
 */
Datum
pg_io_audit_get_stats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	AuditState *state = audit_get_state();

	/* Set up a tuplestore for the result rows. */
	InitMaterializedSRF(fcinfo, 0);

	for (int op = 0; op < AUDIT_NUM_OPS; op++)
	{
		for (int fork = 0; fork <= MAX_FORKNUM; fork++)
		{
			Datum		values[4];
			bool		nulls[4] = {false, false, false, false};
			uint64		calls = pg_atomic_read_u64(&state->counters[op][fork].calls);

			/* Skip combinations that never happened, to keep the view short. */
			if (calls == 0)
				continue;

			values[0] = CStringGetTextDatum(audit_op_names[op]);
			values[1] = CStringGetTextDatum(forkNames[fork]);
			values[2] = Int64GetDatum((int64) calls);
			values[3] = Int64GetDatum((int64)
									  pg_atomic_read_u64(&state->counters[op][fork].blocks));
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}

	return (Datum) 0;
}

/*
 * pg_io_audit_get_trace() -- the most recent traced operations, oldest
 * first.  Backs the view pg_io_audit_trace.  Entries that were overwritten
 * while being read are skipped.
 */
Datum
pg_io_audit_get_trace(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	AuditState *state = audit_get_state();
	uint64		head;
	uint64		first;

	InitMaterializedSRF(fcinfo, 0);

	if (audit_trace_size == 0)
		return (Datum) 0;

	/*
	 * Events are numbered from 0.  The ring holds the last audit_trace_size
	 * of them, so the oldest one still available is head - size.
	 */
	head = pg_atomic_read_u64(&state->trace_head);
	first = head > (uint64) audit_trace_size ? head - audit_trace_size : 0;

	for (uint64 idx = first; idx < head; idx++)
	{
		AuditTraceEntry *slot = &state->trace[idx % audit_trace_size];
		AuditTraceEntry copy;
		uint64		seq;
		Datum		values[10];
		bool		nulls[10] = {false};

		/*
		 * Read the slot like a sequence lock: check that it holds event idx,
		 * copy it, and check again that it still does.  Otherwise a writer
		 * was active in it, and the entry is skipped.
		 */
		seq = pg_atomic_read_u64(&slot->seq);
		if (seq != idx + 1)
			continue;
		pg_read_barrier();
		copy.ts = slot->ts;
		copy.pid = slot->pid;
		copy.op = slot->op;
		copy.fork = slot->fork;
		copy.spcOid = slot->spcOid;
		copy.dbOid = slot->dbOid;
		copy.relNumber = slot->relNumber;
		copy.block = slot->block;
		copy.nblocks = slot->nblocks;
		pg_read_barrier();
		if (pg_atomic_read_u64(&slot->seq) != seq)
			continue;

		values[0] = Int64GetDatum((int64) idx);
		values[1] = TimestampTzGetDatum(copy.ts);
		values[2] = Int32GetDatum(copy.pid);
		values[3] = CStringGetTextDatum(audit_op_names[copy.op]);
		values[4] = CStringGetTextDatum(forkNames[copy.fork]);
		values[5] = ObjectIdGetDatum(copy.spcOid);
		values[6] = ObjectIdGetDatum(copy.dbOid);
		values[7] = ObjectIdGetDatum(copy.relNumber);
		values[8] = Int64GetDatum((int64) copy.block);
		values[9] = Int32GetDatum((int32) copy.nblocks);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/*
 * pg_io_audit_reset() -- zero the counters and empty the trace.  Restricted
 * to superusers: it destroys data that other users (pg_monitor) rely on.
 */
Datum
pg_io_audit_reset(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to reset pg_io_audit")));

	audit_reset_state(audit_get_state());

	PG_RETURN_VOID();
}
