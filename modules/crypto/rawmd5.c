/*
 * Copyright (c) 2009 Atheme Development Group
 * Rights to this code are as documented in doc/LICENSE.
 *
 * Raw MD5 password encryption, as used by e.g. Anope 1.8.
 * Hash functions are not designed to encrypt passwords directly,
 * but we need this to convert some Anope databases.
 */

#include "atheme.h"

#define MODULE_PREFIX_STR       "$rawmd5$"
#define MODULE_PREFIX_LEN       8
#define MODULE_DIGEST_LEN       DIGEST_MDLEN_MD5
#define MODULE_PARAMS_LEN       (MODULE_PREFIX_LEN + (2 * MODULE_DIGEST_LEN))

static bool
atheme_rawmd5_verify(const char *const restrict password, const char *const restrict parameters,
                     unsigned int *const restrict flags)
{
	if (strlen(parameters) != MODULE_PARAMS_LEN)
		return false;

	if (strncmp(parameters, MODULE_PREFIX_STR, MODULE_PREFIX_LEN) != 0)
		return false;

	*flags |= PWVERIFY_FLAG_MYMODULE;

	unsigned char digest[MODULE_DIGEST_LEN];

	if (! digest_oneshot(DIGALG_MD5, password, strlen(password), digest, NULL))
		return false;

	char result[(2 * MODULE_DIGEST_LEN) + 1];

	for (size_t i = 0; i < sizeof digest; i++)
		(void) sprintf(result + i * 2, "%02x", 255 & digest[i]);

	if (strcmp(result, parameters + MODULE_PREFIX_LEN) != 0)
		return false;

	return true;
}

static crypt_impl_t crypto_rawmd5_impl = {

	.id         = "rawmd5",
	.verify     = &atheme_rawmd5_verify,
};

static void
mod_init(module_t __attribute__((unused)) *const restrict m)
{
	(void) crypt_register(&crypto_rawmd5_impl);
}

static void
mod_deinit(const module_unload_intent_t __attribute__((unused)) intent)
{
	(void) crypt_unregister(&crypto_rawmd5_impl);
}

SIMPLE_DECLARE_MODULE_V1("crypto/rawmd5", MODULE_UNLOAD_CAPABILITY_OK)
