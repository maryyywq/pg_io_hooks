/*-------------------------------------------------------------------------
 *
 * pg_page_crypt.c
 *		Transparent encryption of relation files with AES-256-XTS, using
 *		the smgr I/O transformation hooks.
 *
 * Blocks of user relations are encrypted on their way to disk and decrypted
 * when read.  The core computes the page checksum over the plaintext before
 * the write hook runs and verifies it after the read hook has run, so data
 * checksums keep working and are the only integrity check (XTS does not
 * authenticate); data checksums must therefore be enabled.
 *
 * A cluster is converted once, while stopped, with pg_page_crypt_convert.
 * That tool leaves a state file in PGDATA, which this module checks at
 * startup together with the fingerprint of the key, so that a missing
 * conversion or a wrong key stops the server instead of producing garbage.
 *
 * Only this module should transform data; other modules that use the smgr
 * hooks must pass the data through.
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

static char *key_file = NULL;

static PageCipher cipher;

/* Previous hook values */
static smgr_read_hook_type prev_read_hook = NULL;
static smgr_write_hook_type prev_write_hook = NULL;
static smgr_extend_hook_type prev_extend_hook = NULL;

/*
 * Output buffers of the write and extend hooks.  They are static so that
 * nothing is allocated while writing, and aligned for direct I/O.
 */
static PGIOAlignedBlock write_blocks[MAX_IO_COMBINE_LIMIT];
static const void *write_ptrs[MAX_IO_COMBINE_LIMIT];
static PGIOAlignedBlock extend_block;

static bool
is_encrypted(SMgrRelation reln)
{
	return reln->smgr_rlocator.locator.relNumber >= PAGE_CIPHER_FIRST_RELNUMBER;
}

static void
crypt_read_hook(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				void **buffers, BlockNumber nblocks)
{
	if (prev_read_hook)
		prev_read_hook(reln, forknum, blocknum, buffers, nblocks);

	if (!is_encrypted(reln))
		return;

	for (BlockNumber i = 0; i < nblocks; i++)
	{
		if (page_cipher_block_is_zero(buffers[i]))
			continue;
		if (!page_cipher_apply(&cipher, false,
							   reln->smgr_rlocator.locator.relNumber,
							   blocknum + i, buffers[i], buffers[i]))
			ereport(ERROR,
					(errmsg("pg_page_crypt: could not decrypt block %u of relation file %u",
							blocknum + i,
							reln->smgr_rlocator.locator.relNumber)));
	}
}

static const void **
crypt_write_hook(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				 const void **buffers, BlockNumber nblocks)
{
	if (prev_write_hook)
		buffers = prev_write_hook(reln, forknum, blocknum, buffers, nblocks);

	if (!is_encrypted(reln))
		return buffers;

	if (nblocks > MAX_IO_COMBINE_LIMIT)
		elog(ERROR, "pg_page_crypt: too many blocks in one write: %u", nblocks);

	for (BlockNumber i = 0; i < nblocks; i++)
	{
		if (page_cipher_block_is_zero(buffers[i]))
		{
			write_ptrs[i] = buffers[i];
			continue;
		}
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

static const void *
crypt_extend_hook(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				  const void *buffer)
{
	if (prev_extend_hook)
		buffer = prev_extend_hook(reln, forknum, blocknum, buffer);

	if (!is_encrypted(reln) || page_cipher_block_is_zero(buffer))
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
 * Read the key from the file named by pg_page_crypt.key_file.
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
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	buf[n] = '\0';
	FreeFile(fp);

	if (!page_cipher_parse_key(buf, key))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("pg_page_crypt: key file \"%s\" must contain %d hexadecimal characters (two different 256-bit keys)",
						key_file, 2 * PAGE_CIPHER_KEY_LEN)));
	memset(buf, 0, sizeof(buf));
}

/*
 * Check that the cluster was converted with the same key.
 */
static void
check_state(const unsigned char *key)
{
	char		path[MAXPGPATH];
	char		line[128];
	char		fp[PAGE_CIPHER_FP_LEN];
	FILE	   *f;

	snprintf(path, sizeof(path), "%s/%s", DataDir, PAGE_CRYPT_CONVERTING_FILE);
	if (access(path, F_OK) == 0)
		ereport(ERROR,
				(errmsg("pg_page_crypt: an earlier conversion of this cluster did not finish"),
				 errhint("Restore the cluster from a backup; \"%s\" is left over from the interrupted run.",
						 PAGE_CRYPT_CONVERTING_FILE)));

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

	if (strncmp(line, "encrypted ", 10) != 0)
		ereport(ERROR,
				(errmsg("pg_page_crypt: unrecognized contents of \"%s\"", path)));
	if (strncmp(line + 10, fp, PAGE_CIPHER_FP_LEN - 1) != 0)
		ereport(ERROR,
				(errmsg("pg_page_crypt: the key in \"%s\" does not match the key this cluster was encrypted with",
						key_file)));
}

void
_PG_init(void)
{
	unsigned char key[PAGE_CIPHER_KEY_LEN];

	/*
	 * The on-disk format depends on the hooks, so they must be active in
	 * every process from the start.
	 */
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errmsg("pg_page_crypt must be loaded via shared_preload_libraries")));

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
	 * only be noticed through page checksums.
	 */
	if (!DataChecksumsEnabled())
		ereport(ERROR,
				(errmsg("pg_page_crypt requires data checksums to be enabled in this cluster")));

	load_key(key);
	check_state(key);

	if (!page_cipher_init(&cipher, key))
		ereport(ERROR,
				(errmsg("pg_page_crypt: could not initialize the cipher")));
	memset(key, 0, sizeof(key));

	prev_read_hook = smgr_read_hook;
	smgr_read_hook = crypt_read_hook;
	prev_write_hook = smgr_write_hook;
	smgr_write_hook = crypt_write_hook;
	prev_extend_hook = smgr_extend_hook;
	smgr_extend_hook = crypt_extend_hook;
}
