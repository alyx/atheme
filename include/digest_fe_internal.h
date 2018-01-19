/*
 * Internal frontend data structures for the digest interface.
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

#ifndef INC_DIGEST_FE_INTERNAL_H
#define INC_DIGEST_FE_INTERNAL_H

#include "digest_be_md5.h"
#include "digest_be_sha1.h"
#include "digest_be_sha2.h"

union digest_state
{
	struct digest_context_md5       md5ctx;
	struct digest_context_sha1      sha1ctx;
	struct digest_context_sha2_256  sha256ctx;
	struct digest_context_sha2_512  sha512ctx;
};

typedef bool (*digest_init_fn)(union digest_state *);
typedef bool (*digest_update_fn)(union digest_state *, const void *, size_t);
typedef bool (*digest_final_fn)(union digest_state *, void *, size_t *);

struct digest_context
{
	uint8_t                 ikey[DIGEST_BKLEN_MAX];
	uint8_t                 okey[DIGEST_BKLEN_MAX];

	digest_init_fn          init;
	digest_update_fn        update;
	digest_final_fn         final;

	union digest_state      state;
	size_t                  blksz;
	size_t                  digsz;
	bool                    hmac;
};

#endif /* !INC_DIGEST_FE_INTERNAL_H */
