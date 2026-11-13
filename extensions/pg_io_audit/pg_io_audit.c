/*-------------------------------------------------------------------------
 *
 * pg_io_audit.c
 *		Audit of storage I/O operations through the smgr hooks.
 *
 * The extension counts every operation that goes through the storage manager
 * (read, write, extend, zeroextend, sync, truncate, create, unlink) per
 * operation type and fork, and can optionally keep a trace of the most
 * recent operations in a ring buffer.  All data lives in shared memory,
 * because the hooks are called from many processes: backends, the background
 * writer, the checkpointer and the startup process.
 *
 * The hooks only observe.  They call the previous hook and return what it
 * returned, so the extension can be combined with one extension that
 * transforms data.
 *
 * The extension must be loaded with shared_preload_libraries.
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

PG_FUNCTION_INFO_V1(pg_io_audit_get_stats);
PG_FUNCTION_INFO_V1(pg_io_audit_get_trace);
PG_FUNCTION_INFO_V1(pg_io_audit_reset);

void		_PG_init(void);

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

static const char *const audit_op_names[AUDIT_NUM_OPS] = {
	"read", "write", "extend", "zeroextend", "immedsync", "registersync",
	"truncate", "create", "unlink"
};

typedef struct AuditCounter
{
	pg_atomic_uint64 calls;		/* number of calls */
	pg_atomic_uint64 blocks;	/* number of blocks involved */
} AuditCounter;

/*
 * One slot of the trace ring buffer.  "seq" is zero while the slot is being
 * written and idx + 1 once it holds the event number idx; readers use it to
 * discard slots that were overwritten while they were copying them.
 */
typedef struct AuditTraceEntry
{
	pg_atomic_uint64 seq;
	TimestampTz ts;
	int			pid;
	uint8		op;
	uint8		fork;
	Oid			spcOid;
	Oid			dbOid;
	Oid			relNumber;
	BlockNumber block;
	uint32		nblocks;
} AuditTraceEntry;

typedef struct AuditState
{
	pg_atomic_uint64 trace_head;	/* number of events traced so far */
	AuditCounter counters[AUDIT_NUM_OPS][MAX_FORKNUM + 1];
	AuditTraceEntry trace[FLEXIBLE_ARRAY_MEMBER];
} AuditState;

static AuditState *audit_state = NULL;

/* GUCs */
static bool audit_enabled = true;
static int	audit_trace_size = 0;

/* Previous hook values */
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

static Size
audit_memsize(void)
{
	return add_size(offsetof(AuditState, trace),
					mul_size(audit_trace_size, sizeof(AuditTraceEntry)));
}

static void
audit_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(audit_memsize());
}

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
 * entry to the ring buffer.  Tracing is best effort: if the ring is smaller
 * than the number of concurrent writers, a slot can be overwritten while it
 * is being filled, and readers then skip it.
 */
static void
audit_event(AuditOp op, ForkNumber fork, RelFileLocator rlocator,
			BlockNumber block, uint32 nblocks)
{
	AuditState *state = audit_state;

	if (state == NULL || !audit_enabled)
		return;

	pg_atomic_fetch_add_u64(&state->counters[op][fork].calls, 1);
	pg_atomic_fetch_add_u64(&state->counters[op][fork].blocks, nblocks);

	if (audit_trace_size > 0)
	{
		uint64		idx = pg_atomic_fetch_add_u64(&state->trace_head, 1);
		AuditTraceEntry *e = &state->trace[idx % audit_trace_size];

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

static void
audit_sync_hook(SMgrRelation reln, ForkNumber forknum, bool immediate)
{
	if (prev_sync_hook)
		prev_sync_hook(reln, forknum, immediate);

	audit_event(immediate ? AUDIT_SYNC_IMMEDIATE : AUDIT_SYNC_REGISTERED,
				forknum, reln->smgr_rlocator.locator, 0, 0);
}

static void
audit_truncate_hook(SMgrRelation reln, ForkNumber forknum,
					BlockNumber old_nblocks, BlockNumber nblocks)
{
	if (prev_truncate_hook)
		prev_truncate_hook(reln, forknum, old_nblocks, nblocks);

	/* the number of blocks involved is the number removed */
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

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errmsg("pg_io_audit must be loaded via shared_preload_libraries")));

	DefineCustomBoolVariable("pg_io_audit.enabled",
							 "Enables counting of storage I/O operations.",
							 NULL,
							 &audit_enabled,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

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

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = audit_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = audit_shmem_startup;

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
 * seen at least once.
 */
Datum
pg_io_audit_get_stats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	AuditState *state = audit_get_state();

	InitMaterializedSRF(fcinfo, 0);

	for (int op = 0; op < AUDIT_NUM_OPS; op++)
	{
		for (int fork = 0; fork <= MAX_FORKNUM; fork++)
		{
			Datum		values[4];
			bool		nulls[4] = {false, false, false, false};
			uint64		calls = pg_atomic_read_u64(&state->counters[op][fork].calls);

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
 * first.  Entries that were overwritten while being read are skipped.
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

	head = pg_atomic_read_u64(&state->trace_head);
	first = head > (uint64) audit_trace_size ? head - audit_trace_size : 0;

	for (uint64 idx = first; idx < head; idx++)
	{
		AuditTraceEntry *slot = &state->trace[idx % audit_trace_size];
		AuditTraceEntry copy;
		uint64		seq;
		Datum		values[10];
		bool		nulls[10] = {false};

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
 * pg_io_audit_reset() -- zero the counters and empty the trace.
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
