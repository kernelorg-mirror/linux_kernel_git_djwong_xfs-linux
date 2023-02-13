// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SHA512_H__
#define __XFS_SHA512_H__

struct sha512_state {
	union {
		struct shash_desc desc;
		char __desc[sizeof(struct shash_desc) + HASH_MAX_DESCSIZE];
	};
};

#define SHA512_DESC_ON_STACK(mp, name) \
	struct sha512_state name = { .desc.tfm = (mp)->m_sha512 }

#define SHA512_DIGEST_SIZE	64

static inline int sha512_init(struct sha512_state *md)
{
	return crypto_shash_init(&md->desc);
}

static inline int sha512_done(struct sha512_state *md, unsigned char *out)
{
	return crypto_shash_final(&md->desc, out);
}

static inline int sha512_process(struct sha512_state *md,
		const unsigned char *in, unsigned long inlen)
{
	return crypto_shash_update(&md->desc, in, inlen);
}

static inline void sha512_erase(struct sha512_state *md)
{
	memset(md, 0, sizeof(*md));
}

#endif /* __XFS_SHA512_H__ */
