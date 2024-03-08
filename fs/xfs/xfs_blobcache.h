// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_BLOBCACHE_H__
#define __XFS_BLOBCACHE_H__

#ifdef CONFIG_XFS_BLOBCACHE
typedef unsigned long		xfs_blobitem_key_t;

static inline bool xfs_blobitem_key_check(unsigned long long key)
{
	/* xarrays have ulong indices */
	return key <= ULONG_MAX;
}

struct xfs_blobcache {
	/* mapping of unsigned long to blobitem objects */
	struct xarray		bc_mapping;

	/* enough info for tracing data */
	struct xfs_mount	*bc_mount;
	unsigned long long	bc_cookie;

	struct shrinker		*bc_shrinker;
};

struct xfs_blobitem {
	struct rcu_head		bi_rcu;

	/* refcount of this item; the cache holds its own ref */
	refcount_t		bi_refcount;

	/* number of times the shrinker should ignore this item */
	atomic_t		bi_shrinkref;

	/* index key for the mapping */
	xfs_blobitem_key_t	bi_key;

	/* backref to the cache */
	struct xfs_blobcache	*bi_cache;

	/* blob info */
	const void		*bi_data;
	size_t			bi_bytecount;
};

/* cache item functions */
void xfs_blobitem_set_shrinkref(struct xfs_blobitem *bi,
		unsigned int shrinkref);
void xfs_blobitem_rele(struct xfs_blobitem *bi);
int xfs_blobitem_invalidate(struct xfs_blobitem *bi);

/* cache functions */
struct xfs_blobcache *xfs_blobcache_alloc(struct xfs_mount *mp,
		const char *descr, unsigned long long cookie);
void xfs_blobcache_free(struct xfs_blobcache *bc);

struct xfs_blobitem *xfs_blobcache_load(struct xfs_blobcache *bc,
		xfs_blobitem_key_t key, size_t bytecount);
int xfs_blobcache_store(struct xfs_blobcache *bc, xfs_blobitem_key_t key,
		const void *data, size_t bytecount, struct xfs_blobitem **bip);

/* module management */
int __init xfs_blobcache_insmod(void);
void xfs_blobcache_rmmod(void);
#else

# define xfs_blobcache_insmod()		(0)
# define xfs_blobcache_rmmod()		((void)0)

#endif /* CONFIG_XFS_BLOBCACHE */

#endif /* __XFS_BLOBCACHE_H__ */
