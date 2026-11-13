/*-------------------------------------------------------------------------
 *
 * pg_page_crypt_convert.c
 *		Encrypt or decrypt the relation files of a stopped cluster for the
 *		pg_page_crypt extension.
 *
 * Why this tool exists: initdb creates a cluster without the extension, so
 * its files are plaintext, and a plaintext page cannot be told apart from an
 * encrypted one.  The cluster is therefore converted once, while the server
 * is stopped.  After that, every page of an encrypted file is ciphertext and
 * the extension (which checks the state file written here) decrypts it on
 * the fly.
 *
 * What is converted: the same files the extension handles, namely relation
 * files with a file number of 16384 or more (user relations) in "base" and
 * in the tablespace directories, all forks and all segments.  All-zero
 * blocks are left alone.  Other files (system catalogs created by initdb,
 * WAL, SLRUs, temporary relations) are not touched.
 *
 * Safety checks before it starts: it refuses to run if the server is
 * running, if the cluster was not shut down cleanly (otherwise WAL replay at
 * the next start would meet pages in the wrong form), if data checksums are
 * disabled, if the key does not match the one used earlier (for --decrypt),
 * or if the cluster is already in the requested state.
 *
 * The conversion is done in place and is not crash safe: take a backup
 * first.  While it runs, a marker file stays in PGDATA; if the tool is
 * interrupted, the marker remains and the server refuses to start.
 *
 * The tool deliberately uses only standard C, POSIX and OpenSSL (not the
 * PostgreSQL frontend libraries), so it can be built stand-alone.
 *
 * IDENTIFICATION
 *		extensions/pg_page_crypt/pg_page_crypt_convert.c
 *
 *-------------------------------------------------------------------------
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "page_cipher.h"

#define MAXPATH 4096

static const char *progname = "pg_page_crypt_convert";

/* Cipher state and run-wide settings and statistics */
static PageCipher cipher;
static bool do_encrypt = true;
static uint64_t files_done = 0;
static uint64_t blocks_done = 0;

static void
usage(void)
{
	printf("%s encrypts or decrypts the relation files of a stopped cluster for pg_page_crypt.\n\n",
		   progname);
	printf("Usage:\n  %s --encrypt|--decrypt -D DATADIR -k KEYFILE\n\n", progname);
	printf("Options:\n"
		   "  -D, --pgdata=DATADIR   data directory (or set PGDATA)\n"
		   "  -k, --key-file=FILE    file with the key: 128 hexadecimal characters\n"
		   "      --encrypt          encrypt a plaintext cluster\n"
		   "      --decrypt          decrypt an encrypted cluster\n"
		   "  -?, --help             show this help\n");
}

static void fatal(const char *fmt,...) __attribute__((format(printf, 1, 2), noreturn));

/* Print an error message and exit with failure. */
static void
fatal(const char *fmt,...)
{
	va_list		ap;

	fprintf(stderr, "%s: error: ", progname);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	exit(1);
}

static bool
file_exists(const char *path)
{
	return access(path, F_OK) == 0;
}

/*
 * Read the first line of a file into buf; returns false if the file cannot
 * be opened.  An empty file gives an empty string.
 */
static bool
read_first_line(const char *path, char *buf, size_t size)
{
	FILE	   *f = fopen(path, "r");

	if (f == NULL)
		return false;
	if (fgets(buf, size, f) == NULL)
		buf[0] = '\0';
	fclose(f);
	return true;
}

/*
 * Read and parse the key file.  Like the extension, the tool insists that
 * the file is not accessible to group or others.
 */
static void
load_key(const char *path, unsigned char *key)
{
	struct stat st;
	char		buf[2 * PAGE_CIPHER_KEY_LEN + 16];
	FILE	   *f;
	size_t		n;

	if (stat(path, &st) != 0)
		fatal("could not stat key file \"%s\": %s", path, strerror(errno));
	if (!S_ISREG(st.st_mode) || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
		fatal("key file \"%s\" must be a regular file with no access for group or others", path);
	f = fopen(path, "r");
	if (f == NULL)
		fatal("could not open key file \"%s\": %s", path, strerror(errno));
	n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = '\0';
	fclose(f);
	if (!page_cipher_parse_key(buf, key))
		fatal("key file \"%s\" must contain %d hexadecimal characters (two different 256-bit keys)",
			  path, 2 * PAGE_CIPHER_KEY_LEN);
	memset(buf, 0, sizeof(buf));
}

/*
 * The cluster must have been shut down cleanly (otherwise WAL replay at the
 * next start would meet pages in the wrong form) and must have data
 * checksums.  pg_controldata is used to find out, instead of parsing
 * pg_control here, because that file's layout depends on the server version.
 * LC_ALL=C keeps the output in English, so that it can be matched.
 */
static void
check_control(const char *pgdata)
{
	char		cmd[MAXPATH + 64];
	char		line[256];
	FILE	   *p;
	bool		clean = false;
	bool		checksums = false;

	snprintf(cmd, sizeof(cmd), "LC_ALL=C pg_controldata '%s' 2>&1", pgdata);
	p = popen(cmd, "r");
	if (p == NULL)
		fatal("could not run pg_controldata");
	while (fgets(line, sizeof(line), p) != NULL)
	{
		/* Lines look like "Name:      value"; take the value. */
		char	   *val = strchr(line, ':');

		if (val == NULL)
			continue;
		val++;
		while (*val == ' ')
			val++;
		val[strcspn(val, "\n")] = '\0';

		/*
		 * Exactly "shut down": a cluster that was running ("in production"),
		 * crashed, or was a standby ("shut down in recovery") is refused.
		 */
		if (strncmp(line, "Database cluster state:", 23) == 0)
			clean = strcmp(val, "shut down") == 0;
		else if (strncmp(line, "Data page checksum version:", 27) == 0)
			checksums = strcmp(val, "0") != 0;
	}
	if (pclose(p) != 0)
		fatal("pg_controldata failed; is \"%s\" a data directory and is pg_controldata in PATH?", pgdata);
	if (!clean)
		fatal("the cluster was not shut down cleanly; start it and stop it again first");
	if (!checksums)
		fatal("data checksums are not enabled in this cluster, pg_page_crypt requires them");
}

/*
 * Parse a relation file name: "<relnumber>[_fsm|_vm|_init][.<segno>]", for
 * example "16384", "16384_fsm", "16384.1" (the second 1 GB segment).
 * Returns false for any other name: temporary relations ("t3_16384"),
 * "pg_filenode.map", "PG_VERSION" and so on.
 */
static bool
parse_relfile(const char *name, uint32_t *relnumber, uint32_t *segno)
{
	char	   *end;
	unsigned long v;

	if (name[0] < '0' || name[0] > '9')
		return false;
	errno = 0;
	v = strtoul(name, &end, 10);
	if (errno != 0 || v > 0xFFFFFFFFUL)
		return false;
	*relnumber = (uint32_t) v;
	*segno = 0;

	/* Optional fork suffix; the fork itself does not matter for the cipher. */
	if (strncmp(end, "_fsm", 4) == 0)
		end += 4;
	else if (strncmp(end, "_vm", 3) == 0)
		end += 3;
	else if (strncmp(end, "_init", 5) == 0)
		end += 5;

	/* Optional segment number for relations larger than 1 GB. */
	if (*end == '.')
	{
		v = strtoul(end + 1, &end, 10);
		*segno = (uint32_t) v;
	}
	return *end == '\0';
}

/*
 * Convert one relation file, block by block, in place.
 *
 * "segno" is the number of the 1 GB segment this file is, so that the first
 * block of the file has the block number segno * RELSEG_SIZE within the
 * relation; that number goes into the tweak, exactly as it does in the
 * server.
 */
static void
convert_file(const char *path, uint32_t relnumber, uint32_t segno)
{
	unsigned char buf[BLCKSZ];
	int			fd = open(path, O_RDWR);
	off_t		off = 0;
	uint32_t	blkno = segno * (uint32_t) RELSEG_SIZE;

	if (fd < 0)
		fatal("could not open \"%s\": %s", path, strerror(errno));

	for (;;)
	{
		ssize_t		n = pread(fd, buf, BLCKSZ, off);

		if (n == 0)
			break;				/* end of file */
		if (n < 0)
			fatal("could not read \"%s\": %s", path, strerror(errno));
		if (n != BLCKSZ)
			fatal("\"%s\" has a partial block at offset %lld", path, (long long) off);

		/* All-zero blocks are never transformed, as in the server. */
		if (!page_cipher_block_is_zero(buf))
		{
			if (!page_cipher_apply(&cipher, do_encrypt, relnumber, blkno, buf, buf))
				fatal("cipher failure in \"%s\", block %u", path, blkno);
			if (pwrite(fd, buf, BLCKSZ, off) != BLCKSZ)
				fatal("could not write \"%s\": %s", path, strerror(errno));
			blocks_done++;
		}
		off += BLCKSZ;
		blkno++;
	}

	/* Make sure the converted data is on disk before we call it done. */
	if (fsync(fd) != 0)
		fatal("could not fsync \"%s\": %s", path, strerror(errno));
	close(fd);
	files_done++;
}

/*
 * Convert the relation files in one database directory
 * (base/<dboid> or pg_tblspc/<spcoid>/PG_<version>_<catversion>/<dboid>).
 */
static void
convert_dbdir(const char *dir)
{
	DIR		   *d = opendir(dir);
	struct dirent *de;

	if (d == NULL)
		fatal("could not open directory \"%s\": %s", dir, strerror(errno));
	while ((de = readdir(d)) != NULL)
	{
		uint32_t	relnumber;
		uint32_t	segno;
		char		path[MAXPATH];

		if (!parse_relfile(de->d_name, &relnumber, &segno))
			continue;

		/* Same rule as the extension: skip the initdb-created catalogs. */
		if (relnumber < PAGE_CIPHER_FIRST_RELNUMBER)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		convert_file(path, relnumber, segno);
	}
	closedir(d);
}

/*
 * Call fn() for every subdirectory of "dir" whose name starts with
 * "prefix" (an empty prefix matches all).  Symbolic links to directories
 * count: tablespaces in pg_tblspc are links.
 */
static void
for_each_subdir(const char *dir, const char *prefix, void (*fn) (const char *))
{
	DIR		   *d = opendir(dir);
	struct dirent *de;

	if (d == NULL)
		fatal("could not open directory \"%s\": %s", dir, strerror(errno));
	while ((de = readdir(d)) != NULL)
	{
		char		path[MAXPATH];
		struct stat st;

		if (de->d_name[0] == '.' || strncmp(de->d_name, prefix, strlen(prefix)) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			fn(path);
	}
	closedir(d);
}

/* pg_tblspc/<spcoid>/PG_<version>_<catversion>/ contains the database dirs. */
static void
convert_tablespace_version_dir(const char *dir)
{
	for_each_subdir(dir, "", convert_dbdir);
}

/* pg_tblspc/<spcoid>/ contains one PG_* directory. */
static void
convert_tablespace(const char *dir)
{
	for_each_subdir(dir, "PG_", convert_tablespace_version_dir);
}

/*
 * Write the state file: "encrypted <fingerprint of the key>".  It is written
 * to a temporary file, flushed and renamed, so the extension never sees a
 * half-written file.
 */
static void
write_state_file(const char *path, const char *fp)
{
	char		tmp[MAXPATH];
	FILE	   *f;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (f == NULL)
		fatal("could not create \"%s\": %s", tmp, strerror(errno));
	fprintf(f, "encrypted %s\n", fp);
	if (fflush(f) != 0 || fsync(fileno(f)) != 0 || fclose(f) != 0)
		fatal("could not write \"%s\": %s", tmp, strerror(errno));
	if (rename(tmp, path) != 0)
		fatal("could not rename \"%s\": %s", tmp, strerror(errno));
}

int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"pgdata", required_argument, NULL, 'D'},
		{"key-file", required_argument, NULL, 'k'},
		{"encrypt", no_argument, NULL, 1},
		{"decrypt", no_argument, NULL, 2},
		{"help", no_argument, NULL, '?'},
		{NULL, 0, NULL, 0}
	};
	const char *pgdata = getenv("PGDATA");
	const char *keyfile = NULL;
	int			mode = 0;
	int			c;
	unsigned char key[PAGE_CIPHER_KEY_LEN];
	char		fp[PAGE_CIPHER_FP_LEN];
	char		path[MAXPATH];
	char		state_path[MAXPATH];
	char		converting_path[MAXPATH];
	char		line[128];
	FILE	   *f;

	/* ---- Command line ---- */
	while ((c = getopt_long(argc, argv, "D:k:?", long_options, NULL)) != -1)
	{
		switch (c)
		{
			case 'D':
				pgdata = optarg;
				break;
			case 'k':
				keyfile = optarg;
				break;
			case 1:				/* --encrypt */
			case 2:				/* --decrypt */
				if (mode != 0)
					fatal("specify only one of --encrypt and --decrypt");
				mode = c;
				break;
			default:
				usage();
				return c == '?' && optopt == 0 ? 0 : 1;
		}
	}
	if (mode == 0 || pgdata == NULL || keyfile == NULL)
	{
		usage();
		return 1;
	}
	do_encrypt = (mode == 1);

	/* ---- Checks on the cluster ---- */
	snprintf(path, sizeof(path), "%s/PG_VERSION", pgdata);
	if (!read_first_line(path, line, sizeof(line)) || strncmp(line, "17", 2) != 0)
		fatal("\"%s\" is not a PostgreSQL 17 data directory", pgdata);
	snprintf(path, sizeof(path), "%s/postmaster.pid", pgdata);
	if (file_exists(path))
		fatal("the server is running (found \"%s\")", path);

	check_control(pgdata);

	/* ---- Key and conversion state ---- */
	load_key(keyfile, key);
	if (!page_cipher_fingerprint(key, fp) || !page_cipher_init(&cipher, key))
		fatal("could not initialize the cipher");

	snprintf(state_path, sizeof(state_path), "%s/%s", pgdata, PAGE_CRYPT_STATE_FILE);
	snprintf(converting_path, sizeof(converting_path), "%s/%s", pgdata, PAGE_CRYPT_CONVERTING_FILE);
	if (file_exists(converting_path))
		fatal("an earlier conversion did not finish (found \"%s\"); restore the cluster from a backup",
			  converting_path);

	if (do_encrypt)
	{
		/* Encrypting twice would turn ciphertext into something else. */
		if (file_exists(state_path))
			fatal("the cluster is already encrypted (found \"%s\")", state_path);
	}
	else
	{
		/* Decrypting needs the very key that was used to encrypt. */
		if (!read_first_line(state_path, line, sizeof(line)))
			fatal("the cluster is not encrypted (\"%s\" is missing)", state_path);
		if (strncmp(line, "encrypted ", 10) != 0 || strncmp(line + 10, fp, PAGE_CIPHER_FP_LEN - 1) != 0)
			fatal("the key does not match the key this cluster was encrypted with");
	}

	/*
	 * ---- Conversion ----
	 *
	 * Create the marker first: from here on the cluster is in a mixed state
	 * until we finish, and the server must not start on it.
	 */
	f = fopen(converting_path, "w");
	if (f == NULL)
		fatal("could not create \"%s\": %s", converting_path, strerror(errno));
	fclose(f);

	snprintf(path, sizeof(path), "%s/base", pgdata);
	for_each_subdir(path, "", convert_dbdir);
	snprintf(path, sizeof(path), "%s/pg_tblspc", pgdata);
	for_each_subdir(path, "", convert_tablespace);

	/*
	 * ---- Finish ----
	 *
	 * Record the new state first and remove the marker last, so that an
	 * interruption in between never looks like a finished conversion.
	 */
	if (do_encrypt)
		write_state_file(state_path, fp);
	else if (unlink(state_path) != 0)
		fatal("could not remove \"%s\": %s", state_path, strerror(errno));
	if (unlink(converting_path) != 0)
		fatal("could not remove \"%s\": %s", converting_path, strerror(errno));

	printf("%s: %llu files, %llu blocks %s\n", progname,
		   (unsigned long long) files_done, (unsigned long long) blocks_done,
		   do_encrypt ? "encrypted" : "decrypted");
	return 0;
}
