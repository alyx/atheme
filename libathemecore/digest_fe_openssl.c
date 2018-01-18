/*
 * OpenSSL frontend for the digest interface.
 *
 * Copyright (C) 2018 Aaron M. D. Jones <aaronmdjones@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "atheme.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

static inline const EVP_MD *
digest_decide_md(const unsigned int alg)
{
	switch (alg)
	{
		case DIGALG_MD5:
			return EVP_md5();

		case DIGALG_SHA1:
			return EVP_sha1();

		case DIGALG_SHA2_256:
			return EVP_sha256();

		case DIGALG_SHA2_512:
			return EVP_sha512();
	}

	(void) slog(LG_ERROR, "%s: called with unknown/unimplemented alg '%u' (BUG)", __func__, alg);
	return NULL;
}

bool
digest_init(struct digest_context *const restrict ctx, const unsigned int alg)
{
	if (! ctx)
	{
		(void) slog(LG_ERROR, "%s: called with NULL 'ctx' (BUG)", __func__);
		return false;
	}

	(void) memset(ctx, 0x00, sizeof *ctx);
	(void) EVP_MD_CTX_init(&ctx->state.d);

	if (! (ctx->md = digest_decide_md(alg)))
		return false;

	if (EVP_DigestInit_ex(&ctx->state.d, ctx->md, NULL) != 1)
		return false;

	return true;
}

bool
digest_init_hmac(struct digest_context *const restrict ctx, const unsigned int alg,
                 const void *const restrict key, const size_t keyLen)
{
	if (! ctx)
	{
		(void) slog(LG_ERROR, "%s: called with NULL 'ctx' (BUG)", __func__);
		return false;
	}

	(void) memset(ctx, 0x00, sizeof *ctx);
	(void) HMAC_CTX_init(&ctx->state.h);

	ctx->hmac = true;

	if (! (ctx->md = digest_decide_md(alg)))
		return false;

	if (HMAC_Init_ex(&ctx->state.h, key, (int) keyLen, ctx->md, NULL) != 1)
		return false;

	return true;
}

bool
digest_update(struct digest_context *const restrict ctx, const void *const restrict data, const size_t dataLen)
{
	if (! ctx)
	{
		(void) slog(LG_ERROR, "%s: called with NULL 'ctx' (BUG)", __func__);
		return false;
	}

	if (! (data && dataLen))
		return true;

	if (ctx->hmac)
	{
		if (HMAC_Update(&ctx->state.h, data, dataLen) != 1)
			return false;
	}
	else
	{
		if (EVP_DigestUpdate(&ctx->state.d, data, dataLen) != 1)
			return false;
	}

	return true;
}

bool
digest_final(struct digest_context *const restrict ctx, void *const restrict out, size_t *const restrict outLen)
{
	if (! ctx)
	{
		(void) slog(LG_ERROR, "%s: called with NULL 'ctx' (BUG)", __func__);
		return false;
	}
	if (! out)
	{
		(void) slog(LG_ERROR, "%s: called with NULL 'out' (BUG)", __func__);
		return false;
	}

	const size_t hLen = (size_t) EVP_MD_size(ctx->md);
	unsigned int uLen = EVP_MAX_MD_SIZE;

	if (outLen && *outLen < hLen)
	{
		(void) slog(LG_ERROR, "%s: output buffer is too small (BUG)", __func__);
		return false;
	}
	else if (outLen)
		uLen = *outLen;

	if (ctx->hmac)
	{
		if (HMAC_Final(&ctx->state.h, out, &uLen) != 1)
			return false;

		(void) HMAC_CTX_cleanup(&ctx->state.h);
	}
	else
	{
		if (EVP_DigestFinal_ex(&ctx->state.d, out, &uLen) != 1)
			return false;

		(void) EVP_MD_CTX_cleanup(&ctx->state.d);
	}

	if (outLen)
		*outLen = (size_t) uLen;

	return true;
}

bool
digest_oneshot(const unsigned int alg, const void *const restrict data, const size_t dataLen,
               void *const restrict out, size_t *const restrict outLen)
{
	if (! out)
	{
		(void) slog(LG_ERROR, "%s: called with NULL 'out' (BUG)", __func__);
		return false;
	}

	struct digest_context ctx;

	if (! digest_init(&ctx, alg))
		return false;

	if (! digest_update(&ctx, data, dataLen))
		return false;

	if (! digest_final(&ctx, out, outLen))
		return false;

	(void) explicit_bzero(&ctx, sizeof ctx);
	return true;
}

bool
digest_oneshot_hmac(const unsigned int alg, const void *const restrict key, const size_t keyLen,
                    const void *const restrict data, const size_t dataLen, void *const restrict out,
                    size_t *const restrict outLen)
{
	if (! out)
	{
		(void) slog(LG_ERROR, "%s: called with NULL 'out' (BUG)", __func__);
		return false;
	}

	struct digest_context ctx;

	if (! digest_init_hmac(&ctx, alg, key, keyLen))
		return false;

	if (! digest_update(&ctx, data, dataLen))
		return false;

	if (! digest_final(&ctx, out, outLen))
		return false;

	(void) explicit_bzero(&ctx, sizeof ctx);
	return true;
}

bool
digest_pbkdf2_hmac(const unsigned int alg, const void *const restrict pass, const size_t passLen,
                   const void *const restrict salt, const size_t saltLen, const size_t c,
                   void *const restrict dk, const size_t dkLen)
{
	if (! c)
	{
		(void) slog(LG_ERROR, "%s: called with zero 'c' (BUG)", __func__);
		return false;
	}
	if (! dk)
	{
		(void) slog(LG_ERROR, "%s: called with NULL 'dk' (BUG)", __func__);
		return false;
	}
	if (! dkLen)
	{
		(void) slog(LG_ERROR, "%s: called with zero 'dkLen' (BUG)", __func__);
		return false;
	}

	const EVP_MD *const md = digest_decide_md(alg);

	if (! md)
		return false;

	if (PKCS5_PBKDF2_HMAC(pass, (int) passLen, salt, (int) saltLen, (int) c, md, (int) dkLen, dk) != 1)
		return false;

	return true;
}

bool
digest_testsuite_run(void)
{
	(void) slog(LG_DEBUG, "%s: not implemented in OpenSSL frontend", __func__);

	return true;
}
