/*-------------------------------------------------------------------------
 *
 * page_cipher.h
 *		AES-256-XTS encryption of relation blocks, shared by the
 *		pg_page_crypt extension and the pg_page_crypt_convert tool.
 *
 * The same code must be used on both sides, because the tool converts files
 * that the server later reads through the extension.
 *
 * A block is encrypted as a whole.  The XTS tweak is built from the relation
 * file number and the block number, but not from the fork: the server copies
 * the init fork over the main fork of an unlogged relation as plain files
 * (reinit.c), so a block must decrypt the same way in every fork.  Blocks
 * that consist only of zeroes are never transformed; they are how new,
 * unused pages look on disk.
 *
 * IDENTIFICATION
 *		extensions/pg_page_crypt/page_cipher.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PAGE_CIPHER_H
#define PAGE_CIPHER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>

#include "pg_config.h"

/* AES-256-XTS uses two 256-bit keys */
#define PAGE_CIPHER_KEY_LEN 64
/* length of the key fingerprint in hex characters, plus the terminator */
#define PAGE_CIPHER_FP_LEN 17

/* Only relations created after initdb are transformed. */
#define PAGE_CIPHER_FIRST_RELNUMBER 16384

/* Files in PGDATA that describe the state of the conversion */
#define PAGE_CRYPT_STATE_FILE "pg_page_crypt.state"
#define PAGE_CRYPT_CONVERTING_FILE "pg_page_crypt.converting"

typedef struct PageCipher
{
	EVP_CIPHER_CTX *enc;
	EVP_CIPHER_CTX *dec;
} PageCipher;

static inline bool
page_cipher_init(PageCipher *pc, const unsigned char *key)
{
	pc->enc = EVP_CIPHER_CTX_new();
	pc->dec = EVP_CIPHER_CTX_new();
	if (pc->enc == NULL || pc->dec == NULL)
		return false;
	if (EVP_EncryptInit_ex(pc->enc, EVP_aes_256_xts(), NULL, key, NULL) != 1)
		return false;
	if (EVP_DecryptInit_ex(pc->dec, EVP_aes_256_xts(), NULL, key, NULL) != 1)
		return false;
	return true;
}

/*
 * Encrypt or decrypt one block.  "in" and "out" may be the same buffer.
 * Returns false on an OpenSSL failure.
 */
static inline bool
page_cipher_apply(PageCipher *pc, bool encrypt, uint32_t relnumber,
				  uint32_t blkno, const void *in, void *out)
{
	EVP_CIPHER_CTX *ctx = encrypt ? pc->enc : pc->dec;
	unsigned char iv[16];
	int			outl = 0;
	int			finl = 0;

	memset(iv, 0, sizeof(iv));
	for (int i = 0; i < 4; i++)
	{
		iv[i] = (relnumber >> (8 * i)) & 0xFF;
		iv[4 + i] = (blkno >> (8 * i)) & 0xFF;
	}

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
 * white space.  Returns false if the text is not a valid key.
 */
static inline bool
page_cipher_parse_key(const char *text, unsigned char *key)
{
	size_t		len = 0;

	while (text[len] != '\0' && text[len] != '\n' && text[len] != '\r' &&
		   text[len] != ' ')
		len++;
	if (len != 2 * PAGE_CIPHER_KEY_LEN)
		return false;
	for (size_t i = 0; text[len + i] != '\0'; i++)
	{
		if (text[len + i] != '\n' && text[len + i] != '\r' && text[len + i] != ' ')
			return false;
	}

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

	/* XTS requires the two halves of the key to differ */
	return memcmp(key, key + PAGE_CIPHER_KEY_LEN / 2,
				  PAGE_CIPHER_KEY_LEN / 2) != 0;
}

/*
 * Fingerprint of a key: the first 8 bytes of its SHA-256, in hex.  It is
 * stored in the state file to detect a wrong key at startup without
 * revealing the key.
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
