/*-------------------------------------------------------------------------
 *
 * page_cipher.h
 *		AES-256-XTS encryption of relation blocks, shared by the
 *		pg_page_crypt extension and the pg_page_crypt_convert tool.
 *
 * The same code must be used on both sides, because the tool converts files
 * that the server later reads through the extension.  If the two ever
 * disagreed about how a block is encrypted, the server would read garbage
 * (and fail page checksum verification) after a conversion.
 *
 * Why XTS: it is the standard mode for encrypting data at rest in fixed-size
 * units (disk sectors, here 8 kB blocks).  It does not change the size of the
 * data, which matters because a PostgreSQL block cannot grow, and it needs no
 * stored nonce or authentication tag.  The price is that it gives no
 * integrity protection on its own; we rely on page checksums for that.
 *
 * What a block is bound to: the XTS "tweak" (a 16-byte value that makes the
 * same plaintext encrypt differently at different places) is built from the
 * relation file number and the block number, but not from the fork.  The
 * server copies the init fork over the main fork of an unlogged relation as
 * plain files (see reinit.c), so a block must decrypt the same way in every
 * fork.  The tablespace and database OIDs are left out too, so that
 * CREATE DATABASE ... STRATEGY = FILE_COPY, which copies files without going
 * through the storage manager, keeps producing readable relations.
 *
 * Blocks that consist only of zeroes are never transformed.  New, unused
 * pages look like that on disk (relation extension writes zeroes without any
 * hook being involved), and PostgreSQL treats an all-zero page as empty.
 *
 * IDENTIFICATION
 *		extensions/pg_page_crypt/page_cipher.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PAGE_CIPHER_H
#define PAGE_CIPHER_H

/*
 * Only standard headers and pg_config.h are used here, so that this file
 * compiles both inside the server and in the stand-alone conversion tool.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>

#include "pg_config.h"			/* BLCKSZ */

/* AES-256-XTS uses two 256-bit keys */
#define PAGE_CIPHER_KEY_LEN 64
/* length of the key fingerprint in hex characters, plus the terminator */
#define PAGE_CIPHER_FP_LEN 17

/*
 * Only relations created after initdb are transformed.  initdb runs without
 * the extension, so the files of the initial system catalogs (file numbers
 * below 16384, FirstNormalObjectId in the server) stay plaintext.  The rule
 * depends only on the file number, so the server and the conversion tool
 * always agree on which files are encrypted.
 */
#define PAGE_CIPHER_FIRST_RELNUMBER 16384

/*
 * Files in PGDATA that describe the state of the conversion.  The state file
 * exists while the cluster is encrypted; the converting file exists only
 * while the conversion tool is running (or after it was interrupted).
 */
#define PAGE_CRYPT_STATE_FILE "pg_page_crypt.state"
#define PAGE_CRYPT_CONVERTING_FILE "pg_page_crypt.converting"

/*
 * Two OpenSSL contexts, one per direction, each initialized with the key
 * once.  Per block only the tweak is changed.
 */
typedef struct PageCipher
{
	EVP_CIPHER_CTX *enc;
	EVP_CIPHER_CTX *dec;
} PageCipher;

/*
 * Set up the encryption and decryption contexts with the given key
 * (PAGE_CIPHER_KEY_LEN bytes).  Returns false on failure.
 */
static inline bool
page_cipher_init(PageCipher *pc, const unsigned char *key)
{
	pc->enc = EVP_CIPHER_CTX_new();
	pc->dec = EVP_CIPHER_CTX_new();
	if (pc->enc == NULL || pc->dec == NULL)
		return false;

	/* The IV (tweak) is not known yet; it is supplied for every block. */
	if (EVP_EncryptInit_ex(pc->enc, EVP_aes_256_xts(), NULL, key, NULL) != 1)
		return false;
	if (EVP_DecryptInit_ex(pc->dec, EVP_aes_256_xts(), NULL, key, NULL) != 1)
		return false;
	return true;
}

/*
 * Encrypt or decrypt one block of BLCKSZ bytes.  "in" and "out" may be the
 * same buffer.  "relnumber" and "blkno" identify the place of the block
 * (see the comment at the top of the file); the block number is the number
 * within the whole relation, not within a 1 GB segment file.
 *
 * Returns false on an OpenSSL failure.  Nothing here allocates memory in the
 * common case, so this may be called from places where the server forbids
 * allocation.
 */
static inline bool
page_cipher_apply(PageCipher *pc, bool encrypt, uint32_t relnumber,
				  uint32_t blkno, const void *in, void *out)
{
	EVP_CIPHER_CTX *ctx = encrypt ? pc->enc : pc->dec;
	unsigned char iv[16];
	int			outl = 0;
	int			finl = 0;

	/* Tweak: file number and block number, little endian, rest zero. */
	memset(iv, 0, sizeof(iv));
	for (int i = 0; i < 4; i++)
	{
		iv[i] = (relnumber >> (8 * i)) & 0xFF;
		iv[4 + i] = (blkno >> (8 * i)) & 0xFF;
	}

	/*
	 * Passing only the IV re-initializes the context for the next block and
	 * keeps the key set above.  XTS processes the whole block in one update
	 * call and produces no extra bytes in the final call.
	 */
	if (encrypt)
	{
		if (EVP_EncryptInit_ex(ctx, NULL, NULL, NULL, iv) != 1 ||
			EVP_EncryptUpdate(ctx, out, &outl, in, BLCKSZ) != 1 ||
			EVP_EncryptFinal_ex(ctx, (unsigned char *) out + outl, &finl) != 1)
			return false;
	}
	else
	{
		if (EVP_DecryptInit_ex(ctx, NULL, NULL, NULL, iv) != 1 ||
			EVP_DecryptUpdate(ctx, out, &outl, in, BLCKSZ) != 1 ||
			EVP_DecryptFinal_ex(ctx, (unsigned char *) out + outl, &finl) != 1)
			return false;
	}
	return outl + finl == BLCKSZ;
}

/*
 * Is the block entirely zero?  Such blocks are skipped by both the server and
 * the conversion tool.  The block is compared word by word, which is much
 * faster than going byte by byte; BLCKSZ is a multiple of the word size.
 */
static inline bool
page_cipher_block_is_zero(const void *block)
{
	const size_t *p = (const size_t *) block;

	for (size_t i = 0; i < BLCKSZ / sizeof(size_t); i++)
	{
		if (p[i] != 0)
			return false;
	}
	return true;
}

/*
 * Parse a key given as 128 hexadecimal characters, optionally followed by
 * white space (such as the newline at the end of a file).  The result is
 * PAGE_CIPHER_KEY_LEN bytes in "key".  Returns false if the text is not a
 * valid key.
 */
static inline bool
page_cipher_parse_key(const char *text, unsigned char *key)
{
	size_t		len = 0;

	/* The key part ends at the first white space or at the end of the text. */
	while (text[len] != '\0' && text[len] != '\n' && text[len] != '\r' &&
		   text[len] != ' ')
		len++;
	if (len != 2 * PAGE_CIPHER_KEY_LEN)
		return false;

	/* Only white space may follow the key. */
	for (size_t i = 0; text[len + i] != '\0'; i++)
	{
		if (text[len + i] != '\n' && text[len + i] != '\r' && text[len + i] != ' ')
			return false;
	}

	/* Decode two hex digits per byte. */
	for (int i = 0; i < PAGE_CIPHER_KEY_LEN; i++)
	{
		int			v = 0;

		for (int j = 0; j < 2; j++)
		{
			char		c = text[2 * i + j];

			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= c - '0';
			else if (c >= 'a' && c <= 'f')
				v |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				v |= c - 'A' + 10;
			else
				return false;
		}
		key[i] = (unsigned char) v;
	}

	/*
	 * XTS requires its two keys (the halves of the 64 bytes) to differ;
	 * OpenSSL refuses to use a key where they are equal.
	 */
	return memcmp(key, key + PAGE_CIPHER_KEY_LEN / 2,
				  PAGE_CIPHER_KEY_LEN / 2) != 0;
}

/*
 * Fingerprint of a key: the first 8 bytes of its SHA-256, in hex (16
 * characters plus the terminator; "hex" must have room for
 * PAGE_CIPHER_FP_LEN bytes).  It is stored in the state file to detect a
 * wrong key at startup without revealing the key.
 */
static inline bool
page_cipher_fingerprint(const unsigned char *key, char *hex)
{
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int mdlen = 0;

	if (EVP_Digest(key, PAGE_CIPHER_KEY_LEN, md, &mdlen, EVP_sha256(), NULL) != 1)
		return false;
	for (int i = 0; i < 8; i++)
		snprintf(hex + 2 * i, 3, "%02x", md[i]);
	return true;
}

#endif							/* PAGE_CIPHER_H */
