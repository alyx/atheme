/*
 * Copyright (c) 2005 Atheme Development Group
 * Rights to this code are as documented in doc/LICENSE.
 *
 * IRCServices's weird password encryption thingy, taken from Anope 1.6.3.
 */

/*
 * (C) 2003 Anope Team
 * Contact us at info@anope.org
 *
 * Please read COPYING and README for further details.
 *
 * Based on the original code of Epona by Lara.
 * Based on the original code of Services by Andy Church.
 */

#include "atheme.h"

#define MODULE_PREFIX_STR       "$ircservices$"
#define MODULE_PREFIX_LEN       13
#define MODULE_DIGEST_LEN       8
#define MODULE_PARAMS_LEN       (MODULE_PREFIX_LEN + (2 * MODULE_DIGEST_LEN))

#define XTOI(c) ((c)>9 ? (c)-'A'+10 : (c)-'0')

static inline void
atheme_ircservices_encrypt(const char *const restrict src, char *const restrict dest)
{
	char digest[33];
	(void) memset(digest, 0x00, sizeof digest);
	(void) digest_oneshot(DIGALG_MD5, src, strlen(src), digest, NULL);

	char dest2[16];
	for (size_t i = 0; i < 32; i += 2)
		dest2[i / 2] = XTOI(digest[i]) << 4 | XTOI(digest[i + 1]);

	(void) strcpy(dest, MODULE_PREFIX_STR);
	for (size_t i = 0; i < MODULE_DIGEST_LEN; i++)
		(void) sprintf(dest + MODULE_PREFIX_LEN + 2 * i, "%02x", 255 & dest2[i]);
}

static bool
atheme_ircservices_verify(const char *const restrict password, const char *const restrict parameters,
                          unsigned int *const restrict flags)
{
	if (strlen(parameters) != MODULE_PARAMS_LEN)
		return false;

	if (strncmp(parameters, MODULE_PREFIX_STR, MODULE_PREFIX_LEN) != 0)
		return false;

	*flags |= PWVERIFY_FLAG_MYMODULE;

	char result[BUFSIZE];

	(void) atheme_ircservices_encrypt(password, result);

	if (strcmp(result, parameters) != 0)
		return false;

	return true;
}

static crypt_impl_t crypto_ircservices_impl = {

	.id         = "ircservices",
	.verify     = &atheme_ircservices_verify,
};

static void
mod_init(module_t __attribute__((unused)) *const restrict m)
{
	(void) crypt_register(&crypto_ircservices_impl);
}

static void
mod_deinit(const module_unload_intent_t __attribute__((unused)) intent)
{
	(void) crypt_unregister(&crypto_ircservices_impl);
}

SIMPLE_DECLARE_MODULE_V1("crypto/ircservices", MODULE_UNLOAD_CAPABILITY_OK)
