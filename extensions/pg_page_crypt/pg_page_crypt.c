/*-------------------------------------------------------------------------
 *
 * pg_page_crypt.c
 *		Transparent encryption of relation files with AES-256-XTS, using
 *		the smgr I/O transformation hooks.
 *
 * How it fits together
 * --------------------
 * The storage manager (smgr.c) calls three hooks that this module installs:
 *
 *   smgr_write_hook / smgr_extend_hook  just before a block is written to
 *                                       the relation file: encrypt it.
 *   smgr_read_hook                      just after a block was read: decrypt
 *                                       it in place.
 *
 * The rest of the server never sees ciphertext.  Shared buffers always hold
 * plaintext pages, and the page checksum is computed over the plaintext
 * before the write hook runs and verified after the read hook has run.  So
 * the hooks only have to be a cipher and know nothing about checksums, WAL
 * or buffers.
 *
 * Integrity: XTS does not authenticate data.  A modified or wrongly decrypted
 * block decrypts to garbage, and the only thing that notices is the 16-bit
 * page checksum.  Therefore the module refuses to start unless data
 * checksums are enabled.
 *
 * Converting a cluster: initdb runs without this module, so a new cluster
 * starts out as plaintext, and a plaintext page cannot be told apart from
 * ciphertext.  The cluster is therefore converted once, while stopped, by
 * pg_page_crypt_convert.  That tool leaves a state file in PGDATA, which
 * this module checks at startup together with the fingerprint of the key, so
 * that a missing conversion or a wrong key stops the server instead of
 * producing garbage.
 *
 * Only one module may actually change data through these hooks; any other
 * module that uses them must pass the data through (as pg_io_audit does).
 *
 * The module must be loaded with shared_preload_libraries, because the
 * on-disk format depends on it and every process, including the startup
 * process and the checkpointer, must use it from the very beginning.
 *
 * IDENTIFICATION
 *		extensions/pg_page_crypt/pg_page_crypt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#include "access/xlog.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "utils/guc.h"

#include "page_cipher.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

/* Value of the pg_page_crypt.key_file setting */
static char *key_file = NULL;

/*
 * The cipher contexts.  They are created once, in the postmaster, and every
 * other process inherits them through fork().
 */
static PageCipher cipher;

/*
 * Previous values of the hook variables.  We call them first, so that
 * modules loaded before this one keep working (see smgr.h for the rules).
 */
static smgr_read_hook_type prev_read_hook = NULL;
static smgr_write_hook_type prev_write_hook = NULL;
static smgr_extend_hook_type prev_extend_hook = NULL;

/*
 * Output buffers of the write and extend hooks.
 *
 * The write hook must not change the buffer it is given: when data checksums
 * are off that buffer is the live shared buffer, which other processes may
 * be reading.  So it encrypts into a buffer of its own and returns that.
 * The buffers are static, so nothing is allocated at write time, and aligned
 * for direct I/O.  smgr.h guarantees that a single call writes at most
 * MAX_IO_COMBINE_LIMIT blocks, and that the returned buffers only have to
 * stay valid until the next write call in the same process.
 */
static PGIOAlignedBlock write_blocks[MAX_IO_COMBINE_LIMIT];
static const void *write_ptrs[MAX_IO_COMBINE_LIMIT];
static PGIOAlignedBlock extend_block;

/*
 * Is this relation file encrypted?  Only files with a file number of 16384
 * or more are; the files of the initial system catalogs are not, because
 * initdb created them without this module.  The conversion tool uses the same
 * rule (PAGE_CIPHER_FIRST_RELNUMBER).  It does not matter which fork, which
 * database or whether the relation is temporary: the rule looks at the file
 * number alone, which makes it identical on the write and read side.
 */
static bool
is_encrypted(RelFileLocator rlocator)
{
	return rlocator.relNumber >= PAGE_CIPHER_FIRST_RELNUMBER;
}

/*
 * smgr_read_hook: decrypt the blocks that have just been read.
 *
 * This runs before the page checksums are verified, so they are checked
 * against the decrypted pages, as in a server without this module.
 *
 * On PostgreSQL 18 and later this usually runs in the completion callback of
 * an asynchronous read: in a critical section, and possibly in an I/O worker
 * or in another backend.  So it must not raise ERROR or allocate memory.  It
 * only needs the cipher contexts, which every process inherits from the
 * postmaster.  A failure is reported by returning the number of blocks
 * decrypted so far; the core then fails the read.
 */
static BlockNumber
crypt_read_hook(RelFileLocatorBackend rlocator, ForkNumber forknum,
				BlockNumber blocknum, void **buffers, BlockNumber nblocks)
{
	/* Modules loaded before us get to see the data first. */
	if (prev_read_hook)
		nblocks = prev_read_hook(rlocator, forknum, blocknum, buffers, nblocks);

	if (!is_encrypted(rlocator.locator))
		return nblocks;

	for (BlockNumber i = 0; i < nblocks; i++)
	{
		/*
		 * An all-zero block was never encrypted: it is an empty page that was
		 * added to the file by relation extension, or the result of reading
		 * past the end of the file during recovery.
		 */
		if (page_cipher_block_is_zero(buffers[i]))
			continue;

		/* In-place: the read buffers are ours to modify. */
		if (!page_cipher_apply(&cipher, false, rlocator.locator.relNumber,
							   blocknum + i, buffers[i], buffers[i]))
			return i;
	}
	return nblocks;
}

/*
 * smgr_write_hook: encrypt the blocks that smgrwritev() is about to write.
 *
 * Returns the array of buffers to write: our encrypted copies, or the input
 * unchanged for files that are not encrypted.
 */
static const void **
crypt_write_hook(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				 const void **buffers, BlockNumber nblocks)
{
	if (prev_write_hook)
		buffers = prev_write_hook(reln, forknum, blocknum, buffers, nblocks);

	if (!is_encrypted(reln->smgr_rlocator.locator))
		return buffers;

	if (nblocks > MAX_IO_COMBINE_LIMIT)
		elog(ERROR, "pg_page_crypt: too many blocks in one write: %u", nblocks);

	for (BlockNumber i = 0; i < nblocks; i++)
	{
		/* All-zero blocks are written as they are, see crypt_read_hook(). */
		if (page_cipher_block_is_zero(buffers[i]))
		{
			write_ptrs[i] = buffers[i];
			continue;
		}

		/* The input is only read; the result goes to our own buffer. */
		if (!page_cipher_apply(&cipher, true,
							   reln->smgr_rlocator.locator.relNumber,
							   blocknum + i, buffers[i], write_blocks[i].data))
			ereport(ERROR,
					(errmsg("pg_page_crypt: could not encrypt block %u of relation file %u",
							blocknum + i,
							reln->smgr_rlocator.locator.relNumber)));
		write_ptrs[i] = write_blocks[i].data;
	}
	return write_ptrs;
}

/*
 * smgr_extend_hook: the single-block counterpart of crypt_write_hook(), used
 * when a block is added at the end of a file (for example while building an
 * index).  Blocks added as zeroes (smgrzeroextend) do not pass through here
 * at all and stay all-zero on disk.
 */
static const void *
crypt_extend_hook(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				  const void *buffer)
{
	if (prev_extend_hook)
		buffer = prev_extend_hook(reln, forknum, blocknum, buffer);

	if (!is_encrypted(reln->smgr_rlocator.locator) ||
		page_cipher_block_is_zero(buffer))
		return buffer;

	if (!page_cipher_apply(&cipher, true,
						   reln->smgr_rlocator.locator.relNumber,
						   blocknum, buffer, extend_block.data))
		ereport(ERROR,
				(errmsg("pg_page_crypt: could not encrypt block %u of relation file %u",
						blocknum, reln->smgr_rlocator.locator.relNumber)));
	return extend_block.data;
}

/*
 * Read the key from the file named by pg_page_crypt.key_file into "key"
 * (PAGE_CIPHER_KEY_LEN bytes).  Errors out if the file is missing, can be
 * read by group or others, or does not contain a valid key.  The permission
 * check follows the usual rule for secret key files (as for the SSL server
 * key): a key that others can read is not secret.
 */
static void
load_key(unsigned char *key)
{
	struct stat st;
	FILE	   *fp;
	char		buf[2 * PAGE_CIPHER_KEY_LEN + 16];
	size_t		n;

	if (key_file == NULL || key_file[0] == '\0')
		ereport(ERROR,
				(errmsg("pg_page_crypt: pg_page_crypt.key_file is not set")));

	if (stat(key_file, &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("pg_page_crypt: could not stat key file \"%s\": %m",
						key_file)));
	if (!S_ISREG(st.st_mode) || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("pg_page_crypt: key file \"%s\" must be a regular file with no access for group or others",
						key_file)));

	fp = AllocateFile(key_file, "r");
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("pg_page_crypt: could not open key file \"%s\": %m",
						key_file)));

	/* Read a little more than a key, so that trailing garbage is noticed. */
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	buf[n] = '\0';
	FreeFile(fp);

	if (!page_cipher_parse_key(buf, key))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("pg_page_crypt: key file \"%s\" must contain %d hexadecimal characters (two different 256-bit keys)",
						key_file, 2 * PAGE_CIPHER_KEY_LEN)));

	/*
	 * Do not leave the key text lying around on the stack.  explicit_bzero,
	 * unlike memset, is not removed by the optimizer.
	 */
	explicit_bzero(buf, sizeof(buf));
}

/*
 * Check that the cluster in PGDATA was converted, and with the same key as
 * the one we have loaded.  Every failure here stops the server at startup;
 * running with the wrong key would only produce garbage pages.
 */
static void
check_state(const unsigned char *key)
{
	char		path[MAXPGPATH];
	char		line[128];
	char		fp[PAGE_CIPHER_FP_LEN];
	FILE	   *f;

	/* An interrupted conversion leaves files in a mixed state. */
	snprintf(path, sizeof(path), "%s/%s", DataDir, PAGE_CRYPT_CONVERTING_FILE);
	if (access(path, F_OK) == 0)
		ereport(ERROR,
				(errmsg("pg_page_crypt: an earlier conversion of this cluster did not finish"),
				 errhint("Restore the cluster from a backup; \"%s\" is left over from the interrupted run.",
						 PAGE_CRYPT_CONVERTING_FILE)));

	/* The state file is written by pg_page_crypt_convert --encrypt. */
	snprintf(path, sizeof(path), "%s/%s", DataDir, PAGE_CRYPT_STATE_FILE);
	f = AllocateFile(path, "r");
	if (f == NULL)
	{
		if (errno == ENOENT)
			ereport(ERROR,
					(errmsg("pg_page_crypt: this cluster has not been converted"),
					 errhint("Stop the server and run pg_page_crypt_convert --encrypt first.")));
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("pg_page_crypt: could not open \"%s\": %m", path)));
	}
	if (fgets(line, sizeof(line), f) == NULL)
		line[0] = '\0';
	FreeFile(f);

	if (!page_cipher_fingerprint(key, fp))
		ereport(ERROR,
				(errmsg("pg_page_crypt: could not compute the key fingerprint")));

	/* The file has one line: "encrypted <fingerprint of the key>". */
	if (strncmp(line, "encrypted ", 10) != 0)
		ereport(ERROR,
				(errmsg("pg_page_crypt: unrecognized contents of \"%s\"", path)));
	if (strncmp(line + 10, fp, PAGE_CIPHER_FP_LEN - 1) != 0)
		ereport(ERROR,
				(errmsg("pg_page_crypt: the key in \"%s\" does not match the key this cluster was encrypted with",
						key_file)));
}

/*
 * Module initialization.  It runs in the postmaster, while the server is
 * starting, and any ERROR here prevents the server from starting.
 */
void
_PG_init(void)
{
	unsigned char key[PAGE_CIPHER_KEY_LEN];

	/*
	 * The on-disk format depends on the hooks, so they must be active in
	 * every process from the start.  Loading the module later, with LOAD or
	 * in a single session, would leave other processes writing plaintext.
	 */
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errmsg("pg_page_crypt must be loaded via shared_preload_libraries")));

	/*
	 * The setting can only be changed with a server restart: the key is read
	 * once and the cipher keeps using it.
	 */
	DefineCustomStringVariable("pg_page_crypt.key_file",
							   "Path of the file with the encryption key.",
							   "128 hexadecimal characters; the file must not be accessible to group or others.",
							   &key_file,
							   "",
							   PGC_POSTMASTER,
							   0,
							   NULL, NULL, NULL);
	MarkGUCPrefixReserved("pg_page_crypt");

	/*
	 * XTS gives no integrity protection, so corruption and a wrong key can
	 * only be noticed through page checksums.  (pg_control has been read
	 * before shared_preload_libraries is processed, so this is safe here.)
	 */
	if (!DataChecksumsEnabled())
		ereport(ERROR,
				(errmsg("pg_page_crypt requires data checksums to be enabled in this cluster")));

	load_key(key);
	check_state(key);

	if (!page_cipher_init(&cipher, key))
		ereport(ERROR,
				(errmsg("pg_page_crypt: could not initialize the cipher")));
	/* The cipher contexts hold their own copy of the key. */
	explicit_bzero(key, sizeof(key));

	/* Install the hooks, remembering the previous ones to chain to them. */
	prev_read_hook = smgr_read_hook;
	smgr_read_hook = crypt_read_hook;
	prev_write_hook = smgr_write_hook;
	smgr_write_hook = crypt_write_hook;
	prev_extend_hook = smgr_extend_hook;
	smgr_extend_hook = crypt_extend_hook;
}
