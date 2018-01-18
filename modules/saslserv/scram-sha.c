/*
 * SCRAM-SHA-1 and SCRAM-SHA-256 SASL mechanisms provider.
 *
 * See the following RFCs for details:
 *
 *   - RFC 5802 <https://tools.ietf.org/html/rfc5802>
 *     "Salted Challenge Response Authentication Mechanism (SCRAM)"
 *
 *   - RFC 7677 <https://tools.ietf.org/html/rfc7677>
 *     "SCRAM-SHA-256 and SCRAM-SHA-256-PLUS SASL Mechanisms"
 *
 * Copyright (C) 2014 Mantas Mikulėnas <grawity@gmail.com>
 * Copyright (C) 2017-2018 Aaron M. D. Jones <aaronmdjones@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "atheme.h"

#ifdef HAVE_LIBIDN

#include "pbkdf2v2.h"

/* Maximum iteration count Cyrus SASL clients will process
 * Taken from <https://github.com/cyrusimap/cyrus-sasl/blob/f76eb971d456619d0f26/plugins/scram.c#L79>
 */
#define CYRUS_SASL_ITERMAX          0x10000U

#define NONCE_LENGTH                64          // This should be more than sufficient

enum scramsha_step
{
	SCRAMSHA_STEP_CLIENTFIRST   = 0,        // Waiting for client-first-message
	SCRAMSHA_STEP_CLIENTPROOF   = 1,        // Waiting for client-final-message
	SCRAMSHA_STEP_PASSED        = 2,        // Authentication has succeeded
	SCRAMSHA_STEP_FAILED        = 3,        // Authentication has failed
	SCRAMSHA_STEP_ERRORED       = 4,        // Authentication has errored
};

struct scramsha_session
{
	struct pbkdf2v2_dbentry     db;         // Parsed credentials from database
	myuser_t                   *mu;         // User we are authenticating as
	char                       *cn;         // Client nonce
	char                       *sn;         // Server nonce
	char                       *c_gs2_buf;  // Client's GS2 header
	char                       *c_msg_buf;  // Client's first message
	char                       *s_msg_buf;  // Server's first message
	size_t                      c_gs2_len;  // Client's GS2 header (length)
	size_t                      c_msg_len;  // Client's first message (length)
	size_t                      s_msg_len;  // Server's first message (length)
	enum scramsha_step          step;       // What step in authentication are we at?
};

typedef char *scram_attr_list[128];

static const struct sasl_core_functions *sasl_core_functions = NULL;
static const struct pbkdf2v2_scram_functions *pbkdf2v2_scram_functions = NULL;

static bool
sasl_scramsha_attrlist_parse(const char *restrict str, scram_attr_list *const restrict attrs)
{
	(void) memset(*attrs, 0x00, sizeof *attrs);

	for (;;)
	{
		unsigned char name = (unsigned char) *str++;

		if (name < 'A' || (name > 'Z' && name < 'a') || name > 'z')
		{
			// RFC 5802 Section 5: "All attribute names are single US-ASCII letters"
			(void) slog(LG_DEBUG, "%s: invalid attribute name", __func__);
			return false;
		}

		if (*str++ == '=')
		{
			const char *const pos = strchr(str, ',');

			if (pos)
			{
				(*attrs)[name] = sstrndup(str, (size_t) (pos - str));
				(void) slog(LG_DEBUG, "%s: parsed '%c'='%s'", __func__,
				                      (char) name, (*attrs)[name]);
				str = pos + 1;
			}
			else
			{
				(*attrs)[name] = sstrdup(str);
				(void) slog(LG_DEBUG, "%s: parsed '%c'='%s'", __func__,
				                      (char) name, (*attrs)[name]);

				// Reached end of attribute list
				return true;
			}
		}
		else
		{
			(void) slog(LG_DEBUG, "%s: attribute '%c' without value", __func__, (char) name);
			return false;
		}
	}
}

static inline void
sasl_scramsha_attrlist_free(scram_attr_list *const restrict attrs)
{
	for (unsigned char x = 'A'; x <= 'z'; x++)
	{
		(void) free((*attrs)[x]);

		(*attrs)[x] = NULL;
	}
}

static unsigned int
mech_start(struct sasl_session *const restrict p, void __attribute__((unused)) **const restrict out,
           size_t __attribute__((unused)) *const restrict out_len)
{
	p->mechdata = smalloc(sizeof(struct scramsha_session));

	return ASASL_MORE;
}

static unsigned int
mech_step_clientfirst(struct sasl_session *const restrict p, const void *const restrict in, const size_t inlen,
                      void **const restrict out, size_t *const restrict outlen, const unsigned int prf)
{
	if (! (in && inlen))
		return ASASL_ERROR;

	struct scramsha_session *const s = p->mechdata;

	const char *const header = in;
	const char *message = in;

	if (strnlen(in, inlen) != inlen)
	{
		(void) slog(LG_DEBUG, "%s: NULL byte in data received from client", __func__);
		return ASASL_ERROR;
	}

	switch (*message++)
	{
		// RFC 5802 Section 7 (gs2-cbind-flag)
		case 'y':
		case 'n':
			break;

		case 'p':
			(void) slog(LG_DEBUG, "%s: channel binding requested but unsupported", __func__);
			return ASASL_ERROR;

		default:
			(void) slog(LG_DEBUG, "%s: malformed GS2 header (invalid first byte)", __func__);
			return ASASL_ERROR;
	}

	if (*message++ != ',')
	{
		(void) slog(LG_DEBUG, "%s: malformed GS2 header (cbind flag not one letter)", __func__);
		return ASASL_ERROR;
	}

	// Does GS2 header include an authzid ?
	if (message[0] == 'a' && message[1] == '=')
	{
		char authzid[NICKLEN];

		message += 2;

		// Locate its end
		const char *const pos = strchr(message + 1, ',');
		if (! pos)
		{
			(void) slog(LG_DEBUG, "%s: malformed GS2 header (no end to authzid)", __func__);
			return ASASL_ERROR;
		}

		// Check its length
		const size_t authzid_length = (size_t) (pos - message);
		if (authzid_length >= sizeof authzid)
		{
			(void) slog(LG_DEBUG, "%s: unacceptable authzid length '%zu'", __func__, authzid_length);
			return ASASL_ERROR;
		}

		// Copy it
		(void) memset(authzid, 0x00, sizeof authzid);
		(void) memcpy(authzid, message, authzid_length);

		// Normalize it
		if (! pbkdf2v2_scram_functions->normalize(authzid, sizeof authzid))
		{
			(void) slog(LG_DEBUG, "%s: SASLprep normalization of authzid failed", __func__);
			return ASASL_ERROR;
		}

		// Log it
		(void) slog(LG_DEBUG, "%s: parsed authzid '%s'", __func__, authzid);

		// Check it exists and can login
		if (! sasl_core_functions->authzid_can_login(p, authzid, NULL))
		{
			(void) slog(LG_DEBUG, "%s: authzid_can_login failed", __func__);
			return ASASL_ERROR;
		}

		message = pos + 1;
	}
	else if (*message++ != ',')
	{
		(void) slog(LG_DEBUG, "%s: malformed GS2 header (authzid section not empty)", __func__);
		return ASASL_ERROR;
	}

	scram_attr_list input;
	if (! sasl_scramsha_attrlist_parse(message, &input))
		// Malformed SCRAM attribute list
		goto error;

	if (input['m'] || ! (input['n'] && *input['n'] && input['r'] && *input['r']))
	{
		(void) slog(LG_DEBUG, "%s: attribute list unacceptable", __func__);
		goto error;
	}

	char authcid[NICKLEN];
	const size_t authcid_length = strlen(input['n']);

	if (authcid_length >= sizeof authcid)
	{
		(void) slog(LG_DEBUG, "%s: unacceptable authcid length '%zu'", __func__, authcid_length);
		goto error;
	}

	(void) mowgli_strlcpy(authcid, input['n'], sizeof authcid);

	if (! pbkdf2v2_scram_functions->normalize(authcid, sizeof authcid))
	{
		(void) slog(LG_DEBUG, "%s: SASLprep normalization of authcid failed", __func__);
		goto error;
	}

	(void) slog(LG_DEBUG, "%s: parsed authcid '%s'", __func__, authcid);

	if (! sasl_core_functions->authcid_can_login(p, authcid, &s->mu))
	{
		(void) slog(LG_DEBUG, "%s: authcid_can_login failed", __func__);
		goto error;
	}

	if (! (s->mu->flags & MU_CRYPTPASS))
	{
		(void) slog(LG_DEBUG, "%s: user's password is not encrypted", __func__);
		goto error;
	}

	if (s->mu->flags & MU_NOPASSWORD)
	{
		(void) slog(LG_DEBUG, "%s: user has NOPASSWORD flag set", __func__);
		goto error;
	}

	if (! pbkdf2v2_scram_functions->dbextract(s->mu->pass, &s->db))
		// User's password hash is not in a compatible (PBKDF2 v2) format
		goto error;

	if (s->db.a != prf)
	{
		(void) slog(LG_DEBUG, "%s: PRF ID mismatch: server(%u) != client(%u)", __func__, s->db.a, prf);
		goto error;
	}

	// These cannot fail
	s->c_gs2_len = (size_t) (message - header);
	s->c_gs2_buf = sstrndup(header, s->c_gs2_len);
	s->c_msg_len = inlen - s->c_gs2_len;
	s->c_msg_buf = sstrndup(message, s->c_msg_len);
	s->cn = sstrdup(input['r']);
	s->sn = random_string(NONCE_LENGTH);

	// Construct server-first-message
	char response[SASL_C2S_MAXLEN];
	const int ol = snprintf(response, sizeof response, "r=%s%s,s=%s,i=%u", s->cn, s->sn, s->db.salt64, s->db.c);

	if (ol <= (int)(NONCE_LENGTH + PBKDF2_SALTLEN_MIN + 16) || ol >= (int) sizeof response)
	{
		(void) slog(LG_ERROR, "%s: snprintf(3) for server-first-message failed", __func__);
		goto error;
	}

	// Cannot fail
	*outlen = (size_t) ol;
	*out = smalloc(*outlen);
	(void) memcpy(*out, response, *outlen);

	s->s_msg_len = *outlen;
	s->s_msg_buf = sstrdup(response);

	(void) sasl_scramsha_attrlist_free(&input);
	s->step = SCRAMSHA_STEP_CLIENTPROOF;
	return ASASL_MORE;

error:
	(void) sasl_scramsha_attrlist_free(&input);
	s->step = SCRAMSHA_STEP_ERRORED;
	return ASASL_ERROR;
}

static unsigned int
mech_step_clientproof(struct scramsha_session *const restrict s, const void *const restrict in, const size_t inlen,
                      void **const restrict out, size_t *const restrict outlen)
{
	if (! (in && inlen))
		return ASASL_ERROR;

	if (strnlen(in, inlen) != inlen)
	{
		(void) slog(LG_DEBUG, "%s: NULL byte in data received from client", __func__);
		return ASASL_ERROR;
	}

	scram_attr_list input;
	if (! sasl_scramsha_attrlist_parse(in, &input))
		// Malformed SCRAM attribute list
		goto error;

	if (input['m'] || ! (input['c'] && *input['c'] && input['p'] && *input['p'] && input['r'] && *input['r']))
	{
		(void) slog(LG_DEBUG, "%s: attribute list unacceptable", __func__);
		goto error;
	}

	// Concatenate the s-nonce to the c-nonce
	char x_nonce[SASL_C2S_MAXLEN];
	const int xl = snprintf(x_nonce, sizeof x_nonce, "%s%s", s->cn, s->sn);
	if (xl <= NONCE_LENGTH || xl >= (int) sizeof x_nonce)
	{
		(void) slog(LG_ERROR, "%s: snprintf(3) for concatenated salts failed (BUG?)", __func__);
		goto error;
	}
	if (strcmp(x_nonce, input['r']) != 0)
	{
		(void) slog(LG_DEBUG, "%s: nonce sent by client doesn't match nonce we sent", __func__);
		goto error;
	}

	// Decode GS2 header from client-final-message
	char c_gs2_buf[SASL_C2S_MAXLEN];
	const size_t c_gs2_len = base64_decode(input['c'], c_gs2_buf, sizeof c_gs2_buf);
	if (c_gs2_len == (size_t) -1)
	{
		(void) slog(LG_DEBUG, "%s: base64_decode() for GS2 header failed", __func__);
		goto error;
	}
	if (c_gs2_len != s->c_gs2_len || memcmp(s->c_gs2_buf, c_gs2_buf, c_gs2_len) != 0)
	{
		(void) slog(LG_DEBUG, "%s: GS2 header mismatch", __func__);
		goto error;
	}

	// Decode ClientProof from client-final-message
	unsigned char ClientProof[DIGEST_MDLEN_MAX];
	if (base64_decode(input['p'], ClientProof, sizeof ClientProof) != s->db.dl)
	{
		(void) slog(LG_DEBUG, "%s: base64_decode() for ClientProof failed", __func__);
		goto error;
	}

	// Construct AuthMessage
	char AuthMessage[SASL_C2S_MAXLEN];
	const int alen = snprintf(AuthMessage, sizeof AuthMessage, "%s,%s,c=%s,r=%s",
	                          s->c_msg_buf, s->s_msg_buf, input['c'], input['r']);

	if (alen < NONCE_LENGTH || alen >= (int) sizeof AuthMessage)
	{
		(void) slog(LG_ERROR, "%s: snprintf(3) for AuthMessage failed (BUG?)", __func__);
		goto error;
	}

	const unsigned char *const AuthMessageR = (const unsigned char *) AuthMessage;

	// Calculate ClientSignature
	unsigned char ClientSignature[DIGEST_MDLEN_MAX];
	if (! digest_oneshot_hmac(s->db.alg, s->db.shk, s->db.dl, AuthMessageR, (size_t) alen, ClientSignature, NULL))
	{
		(void) slog(LG_ERROR, "%s: digest_oneshot_hmac() for ClientSignature failed", __func__);
		goto error;
	}

	// XOR ClientProof with calculated ClientSignature to derive ClientKey
	unsigned char ClientKey[DIGEST_MDLEN_MAX];
	for (size_t x = 0; x < s->db.dl; x++)
		ClientKey[x] = ClientProof[x] ^ ClientSignature[x];

	// Compute StoredKey from derived ClientKey
	unsigned char StoredKey[DIGEST_MDLEN_MAX];
	if (! digest_oneshot(s->db.alg, ClientKey, s->db.dl, StoredKey, NULL))
	{
		(void) slog(LG_ERROR, "%s: digest_oneshot() for StoredKey failed", __func__);
		goto error;
	}

	// Check computed StoredKey matches the database StoredKey
	if (memcmp(StoredKey, s->db.shk, s->db.dl) != 0)
	{
		(void) slog(LG_DEBUG, "%s: memcmp(3) mismatch on StoredKey; incorrect password?", __func__);
		goto fail;
	}

	/* ******************************************************** *
	 * AUTHENTICATION OF THE CLIENT HAS SUCCEEDED AT THIS POINT *
	 * ******************************************************** */

	// Calculate ServerSignature
	unsigned char ServerSignature[DIGEST_MDLEN_MAX];
	if (! digest_oneshot_hmac(s->db.alg, s->db.ssk, s->db.dl, AuthMessageR, (size_t) alen, ServerSignature, NULL))
	{
		(void) slog(LG_ERROR, "%s: digest_oneshot_hmac() for ServerSignature failed", __func__);
		goto error;
	}

	// Encode ServerSignature
	char ServerSignature64[DIGEST_MDLEN_MAX * 3];
	const size_t rs = base64_encode(ServerSignature, s->db.dl, ServerSignature64, sizeof ServerSignature64);
	if (rs == (size_t) -1)
	{
		(void) slog(LG_ERROR, "%s: base64_encode() for ServerSignature failed", __func__);
		goto error;
	}

	// Construct server-final-message
	char response[DIGEST_MDLEN_MAX * 3];
	const int ol = snprintf(response, sizeof response, "v=%s", ServerSignature64);
	if (ol < (int) s->db.dl || ol >= (int) sizeof response)
	{
		(void) slog(LG_ERROR, "%s: snprintf(3) for response failed (BUG?)", __func__);
		goto error;
	}

	// Cannot fail
	*outlen = (size_t) ol;
	*out = smalloc(*outlen);
	(void) memcpy(*out, response, *outlen);

	(void) sasl_scramsha_attrlist_free(&input);
	s->step = SCRAMSHA_STEP_PASSED;
	return ASASL_MORE;

error:
	(void) sasl_scramsha_attrlist_free(&input);
	s->step = SCRAMSHA_STEP_ERRORED;
	return ASASL_ERROR;

fail:
	(void) sasl_scramsha_attrlist_free(&input);
	s->step = SCRAMSHA_STEP_FAILED;
	return ASASL_FAIL;
}

static unsigned int
mech_step_success(const struct scramsha_session *const restrict s)
{
	if (s->db.scram)
		// User's password hash was already in SCRAM format, nothing to do
		return ASASL_DONE;

	/*
	 * An SASL SCRAM-SHA login has succeeded, but the user's database hash was not in SCRAM format.
	 *
	 * If the database is breached in the future, the raw PBKDF2 digest ("SaltedPassword" in RFC 5802)
	 * can be used to compute the ClientKey and impersonate the client to this service.
	 *
	 * Convert the SaltedPassword into a ServerKey and StoredKey now, and then write those back to the
	 * database, overwriting SaltedPassword. Verification of plaintext passwords can still take place
	 * (e.g. for SASL PLAIN or NickServ IDENTIFY) because modules/crypto/pbkdf2v2 can compute ServerKey
	 * from the provided plaintext password and compare it to the stored ServerKey.
	 */

	(void) slog(LG_INFO, "%s: login succeeded, attempting to convert user's hash to SCRAM format", __func__);

	char csk64[DIGEST_MDLEN_MAX * 3];
	char chk64[DIGEST_MDLEN_MAX * 3];
	char res[PASSLEN];

	if (base64_encode(s->db.ssk, s->db.dl, csk64, sizeof csk64) == (size_t) -1)
	{
		(void) slog(LG_ERROR, "%s: base64_encode for ssk failed", __func__);
	}
	else if (base64_encode(s->db.shk, s->db.dl, chk64, sizeof chk64) == (size_t) -1)
	{
		(void) slog(LG_ERROR, "%s: base64_encode for shk failed", __func__);
	}
	else if (snprintf(res, PASSLEN, PBKDF2_FS_SAVEHASH, s->db.a, s->db.c, s->db.salt64, csk64, chk64) >= PASSLEN)
	{
		(void) slog(LG_ERROR, "%s: snprintf(3) would have overflowed result buffer (BUG)", __func__);
	}
	else if (strlen(res) < (sizeof(PBKDF2_FS_SAVEHASH) + PBKDF2_SALTLEN_MIN + (2 * s->db.dl)))
	{
		(void) slog(LG_ERROR, "%s: snprintf(3) didn't write enough data (BUG?)", __func__);
	}
	else
	{
		(void) mowgli_strlcpy(s->mu->pass, res, PASSLEN);
		(void) slog(LG_DEBUG, "%s: succeeded", __func__);
	}

	return ASASL_DONE;
}

static inline unsigned int
mech_step_dispatch(struct sasl_session *const restrict p, const void *const restrict in, const size_t inlen,
                   void **const restrict out, size_t *const restrict outlen, const unsigned int prf)
{
	if (! (p && p->mechdata))
		return ASASL_ERROR;

	struct scramsha_session *const s = p->mechdata;

	switch (s->step)
	{
		case SCRAMSHA_STEP_CLIENTFIRST:
			return mech_step_clientfirst(p, in, inlen, out, outlen, prf);

		case SCRAMSHA_STEP_CLIENTPROOF:
			return mech_step_clientproof(s, in, inlen, out, outlen);

		case SCRAMSHA_STEP_PASSED:
			return mech_step_success(s);

		case SCRAMSHA_STEP_FAILED:
			return ASASL_FAIL;

		case SCRAMSHA_STEP_ERRORED:
			return ASASL_ERROR;
	}
}

static unsigned int
mech_step_sha1(struct sasl_session *const restrict p, const void *const restrict in, const size_t inlen,
               void **const restrict out, size_t *const restrict outlen)
{
	return mech_step_dispatch(p, in, inlen, out, outlen, PBKDF2_PRF_SCRAM_SHA1_S64);
}

static unsigned int
mech_step_sha2_256(struct sasl_session *const restrict p, const void *const restrict in, const size_t inlen,
                   void **const restrict out, size_t *const restrict outlen)
{
	return mech_step_dispatch(p, in, inlen, out, outlen, PBKDF2_PRF_SCRAM_SHA2_256_S64);
}

static void
mech_finish(struct sasl_session *const restrict p)
{
	if (! (p && p->mechdata))
		return;

	struct scramsha_session *const s = p->mechdata;

	(void) free(s->cn);
	(void) free(s->sn);
	(void) free(s->c_gs2_buf);
	(void) free(s->c_msg_buf);
	(void) free(s->s_msg_buf);
	(void) free(s);

	p->mechdata = NULL;
}

static struct sasl_mechanism sasl_scramsha_mech_sha1 = {

	.name           = "SCRAM-SHA-1",
	.mech_start     = &mech_start,
	.mech_step      = &mech_step_sha1,
	.mech_finish    = &mech_finish,
};

static struct sasl_mechanism sasl_scramsha_mech_sha2_256 = {

	.name           = "SCRAM-SHA-256",
	.mech_start     = &mech_start,
	.mech_step      = &mech_step_sha2_256,
	.mech_finish    = &mech_finish,
};

static inline void
sasl_scramsha_mechs_unregister(void)
{
	(void) sasl_core_functions->mech_unregister(&sasl_scramsha_mech_sha1);
	(void) sasl_core_functions->mech_unregister(&sasl_scramsha_mech_sha2_256);
}

static void
sasl_scramsha_pbkdf2v2_confhook(const unsigned int prf, const unsigned int iter, const unsigned int saltlen)
{
	const crypt_impl_t *const ci_default = crypt_get_default_provider();

	if (! ci_default)
	{
		(void) slog(LG_ERROR, "%s: %s is apparently loaded but no crypto provider is available (BUG)",
		                      __func__, PBKDF2V2_CRYPTO_MODULE_NAME);
	}
	else if (strcmp(ci_default->id, "pbkdf2v2") != 0)
	{
		(void) slog(LG_INFO, "%s: %s is not the default crypto provider, PLEASE INVESTIGATE THIS! "
		                     "Newly registered users, and users who change their passwords, will not "
		                     "be able to login with this module until this is rectified.", __func__,
		                     PBKDF2V2_CRYPTO_MODULE_NAME);
	}

	(void) sasl_scramsha_mechs_unregister();

	switch (prf)
	{
		case PBKDF2_PRF_SCRAM_SHA1_S64:
			(void) sasl_core_functions->mech_register(&sasl_scramsha_mech_sha1);
			break;

		case PBKDF2_PRF_SCRAM_SHA2_256_S64:
			(void) sasl_core_functions->mech_register(&sasl_scramsha_mech_sha2_256);
			break;

		default:
			(void) slog(LG_ERROR, "%s: pbkdf2v2::digest is not set to a supported value -- "
			                      "this module will not do anything", __func__);
			return;
	}

	if (iter > CYRUS_SASL_ITERMAX)
		(void) slog(LG_INFO, "%s: iteration count (%u) is higher than Cyrus SASL library maximum (%u) -- "
		                     "client logins may fail if they use Cyrus", __func__, iter, CYRUS_SASL_ITERMAX);
}

static void
mod_init(module_t *const restrict m)
{
	// Services administrators using this module should be fully aware of the requirements for correctly doing so
	if (! module_find_published(PBKDF2V2_CRYPTO_MODULE_NAME))
	{
		(void) slog(LG_ERROR, "module %s needs module %s", m->name, PBKDF2V2_CRYPTO_MODULE_NAME);

		m->mflags = MODTYPE_FAIL;
		return;
	}

	MODULE_TRY_REQUEST_SYMBOL(m, sasl_core_functions, "saslserv/main", "sasl_core_functions");
	MODULE_TRY_REQUEST_SYMBOL(m, pbkdf2v2_scram_functions, PBKDF2V2_CRYPTO_MODULE_NAME, "pbkdf2v2_scram_functions");

	/* Pass our funcptr to the pbkdf2v2 module, which will immediately call us back with its
	 * configuration. We use its configuration to decide which SASL mechanism to register.
	 */
	(void) pbkdf2v2_scram_functions->confhook(&sasl_scramsha_pbkdf2v2_confhook);
}

static void
mod_deinit(const module_unload_intent_t __attribute__((unused)) intent)
{
	// Unregister configuration interest in the pbkdf2v2 module
	(void) pbkdf2v2_scram_functions->confhook(NULL);

	// Unregister all SASL mechanisms
	(void) sasl_scramsha_mechs_unregister();
}

SIMPLE_DECLARE_MODULE_V1("saslserv/scram-sha", MODULE_UNLOAD_CAPABILITY_OK)

#endif /* HAVE_LIBIDN */
