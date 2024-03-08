// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_trace.h"
#include "xfs_blobcache.h"

static struct kmem_cache	*xfs_blobcache_objs;
static struct kmem_cache	*xfs_blobitem_objs;

/*
 * Set the shrinker refcount on a blob item.  The higher the shrinkref, the
 * longer it survives reclamation.
 */
void
xfs_blobitem_set_shrinkref(
	struct xfs_blobitem	*bi,
	unsigned int		shrinkref)
{
	atomic_set(&bi->bi_shrinkref, shrinkref);
}

/* Free a blob cache item and the blob. */
static void
xfs_blobitem_free(
	struct rcu_head		*head)
{
	struct xfs_blobitem	*bi;

	bi = container_of(head, struct xfs_blobitem, bi_rcu);

	trace_xfs_blobitem_free(bi, _RET_IP_);

	ASSERT(refcount_read(&bi->bi_refcount) == 0);
	kvfree(bi->bi_data);
	kmem_cache_free(xfs_blobitem_objs, bi);
}

/* Release a blob cache item. */
void
xfs_blobitem_rele(
	struct xfs_blobitem	*bi)
{
	trace_xfs_blobitem_rele(bi, _RET_IP_);

	if (refcount_dec_and_test(&bi->bi_refcount))
		call_rcu(&bi->bi_rcu, xfs_blobitem_free);
}

/*
 * Remove an item from the cache.  Returns 0 on success or -ENOENT if this item
 * was not in the cache.  The cache item and the blob will be freed when all
 * active references are dropped, including the one that the caller holds.
 */
int
xfs_blobitem_invalidate(
	struct xfs_blobitem	*bi)
{
	struct xfs_blobcache	*bc = bi->bi_cache;
	struct xfs_blobitem	*old;

	trace_xfs_blobitem_invalidate(bi, _RET_IP_);

	xa_lock(&bc->bc_mapping);
	old = __xa_cmpxchg(&bc->bc_mapping, bi->bi_key, bi, NULL, GFP_KERNEL);
	xa_unlock(&bc->bc_mapping);

	if (old != bi)
		return -ENOENT;

	atomic_set(&bi->bi_shrinkref, 0);
	return 0;
}

/* Drain all the inactive cache items out of the cache. */
static void
xfs_blobcache_drain(
	struct xfs_blobcache	*bc)
{
	XA_STATE(xas, &bc->bc_mapping, 0);
	struct xfs_blobitem	*bi;

	xas_lock(&xas);
	xas_for_each(&xas, bi, ULONG_MAX) {
		ASSERT(refcount_read(&bi->bi_refcount) == 1);

		xas_store(&xas, NULL);
		xfs_blobitem_rele(bi);
	}
	xas_unlock(&xas);
}

/* Reclaim inactive blob cache items that have run out of second chances. */
static unsigned long
xfs_blobcache_shrink_scan(
	struct shrinker		*shrink,
	struct shrink_control	*sc)
{
	struct xfs_blobcache	*bc = shrink->private_data;
	XA_STATE(xas, &bc->bc_mapping, 0);
	struct xfs_blobitem	*bi;
	unsigned long		freed = 0;

	trace_xfs_blobcache_shrink_scan(bc, sc->nr_to_scan);

	xas_lock(&xas);
	xas_for_each(&xas, bi, ULONG_MAX) {
		/* Retain if there are active references */
		if (refcount_read(&bi->bi_refcount) > 1)
			continue;

		/* Ignore if the item still has lru refcount */
		if (atomic_add_unless(&bi->bi_shrinkref, -1, 0))
			continue;

		trace_xfs_blobitem_shrink(bi, _RET_IP_);

		freed++;
		xas_store(&xas, NULL);
		xfs_blobitem_rele(bi);

		if (freed >= sc->nr_to_scan)
			break;
	}
	xas_unlock(&xas);

	trace_xfs_blobcache_shrink_freed(bc, freed);
	return freed;
}

/* Count the number of inactive blob cache items that could be reclaimed. */
static unsigned long
xfs_blobcache_shrink_count(
	struct shrinker		*shrink,
	struct shrink_control	*sc)
{
	struct xfs_blobcache	*bc = shrink->private_data;
	XA_STATE(xas, &bc->bc_mapping, 0);
	struct xfs_blobitem	*bi;
	unsigned long		count = 0;

	trace_xfs_blobcache_shrink_count(bc, _RET_IP_);

	rcu_read_lock();
	xas_for_each(&xas, bi, ULONG_MAX) {
		if (refcount_read(&bi->bi_refcount) == 1)
			count++;
	}
	rcu_read_unlock();

	trace_xfs_blobcache_shrink_counted(bc, count);
	return count;
}

/* Create a cache for memory blobs. */
struct xfs_blobcache *
xfs_blobcache_alloc(
	struct xfs_mount	*mp,
	const char		*descr,
	unsigned long long	cookie)
{
	struct xfs_blobcache	*bc;

	bc = kmem_cache_zalloc(xfs_blobcache_objs, GFP_KERNEL);
	if (!bc)
		return ERR_PTR(-ENOMEM);

	bc->bc_mount = mp;
	bc->bc_cookie = cookie;
	xa_init(&bc->bc_mapping);

	bc->bc_shrinker = shrinker_alloc(SHRINKER_NUMA_AWARE, "xfs_%s:%s-0x%llx",
			descr, mp->m_super->s_id, cookie);
	if (!bc->bc_shrinker)
		goto out_xarray;

	bc->bc_shrinker->count_objects = xfs_blobcache_shrink_count;
	bc->bc_shrinker->scan_objects = xfs_blobcache_shrink_scan;
	bc->bc_shrinker->private_data = bc;
	shrinker_register(bc->bc_shrinker);

	trace_xfs_blobcache_alloc(bc, _RET_IP_);

	return bc;

out_xarray:
	xa_destroy(&bc->bc_mapping);
	kmem_cache_free(xfs_blobcache_objs, bc);
	return ERR_PTR(-ENOMEM);
}

/*
 * Destroy a cache of memory blobs.  Caller must ensure that there are no
 * active references to any cached items.
 */
void
xfs_blobcache_free(
	struct xfs_blobcache	*bc)
{
	trace_xfs_blobcache_free(bc, _RET_IP_);

	xfs_blobcache_drain(bc);
	shrinker_free(bc->bc_shrinker);
	xa_destroy(&bc->bc_mapping);
	kmem_cache_free(xfs_blobcache_objs, bc);
}

/*
 * Return the blob item for a cached object, NULL if not found, or -EINVAL if
 * the size doesn't match the cached blob.
 */
struct xfs_blobitem *
xfs_blobcache_load(
	struct xfs_blobcache	*bc,
	xfs_blobitem_key_t	key,
	size_t			bytecount)
{
	XA_STATE(xas, &bc->bc_mapping, key);
	struct xfs_blobitem	*bi;

	/* Look up the blob item; if one is found, try to get an active ref. */
	rcu_read_lock();
	do {
		bi = xas_load(&xas);
		if (xa_is_zero(bi))
			bi = NULL;
	} while (xas_retry(&xas, bi) ||
		 (bi && !refcount_inc_not_zero(&bi->bi_refcount)));
	rcu_read_unlock();

	if (!bi)
		return NULL;

	/* Don't return anything if the buffer size doesn't match. */
	if (bi->bi_bytecount != bytecount) {
		xfs_blobitem_rele(bi);
		return ERR_PTR(-EINVAL);
	}

	trace_xfs_blobitem_load(bi, _RET_IP_);
	return bi;
}

/*
 * Try to store a blob in the cache with the given key.
 *
 * If the blob is mapped into the cache, the blob item takes ownership of
 * @data.  The new blob item is passed out with an active reference via @bip.
 * The return value is 0.
 *
 * If there was already a blob in the cache but its size does not match,
 * -EINVAL is returned.  The caller retains its ownership of @data.
 *
 * If there was already a blob in the cache and its size matches, the blob
 * item for the existing item is passed out with an active reference via @bip.
 * The caller retains its ownership of @data.  The return value is -EEXIST.
 *
 * If any other error occurs, that value is returned and the caller retains
 * its ownership of @data.
 */
int
xfs_blobcache_store(
	struct xfs_blobcache	*bc,
	xfs_blobitem_key_t	key,
	const void		*data,
	size_t			bytecount,
	struct xfs_blobitem	**bip)
{
	struct xfs_blobitem	*bi, *old;
	int			error;

	*bip = NULL;

	bi = kmem_cache_zalloc(xfs_blobitem_objs, GFP_KERNEL);
	if (!bi)
		return -ENOMEM;

	/* Cache owns the first refcount, and we own the second. */
	refcount_set(&bi->bi_refcount, 2);
	bi->bi_key = key;
	bi->bi_cache = bc;
	bi->bi_data = data;
	bi->bi_bytecount = bytecount;

	trace_xfs_blobitem_store(bi, _RET_IP_);

	/*
	 * Either replace a NULL entry with bi, or take an active ref to
	 * whatever's currently there.
	 */
	xa_lock(&bc->bc_mapping);
	do {
		old = __xa_cmpxchg(&bc->bc_mapping, key, NULL, bi, GFP_KERNEL);
	} while (old && !refcount_inc_not_zero(&old->bi_refcount));
	xa_unlock(&bc->bc_mapping);

	if (old == NULL) {
		/*
		 * There was no previous value.  @bi is now live in the cache,
		 * so pass bi and its active ref to the caller.
		 */
		*bip = bi;
		return 0;
	}

	trace_xfs_blobitem_existing(old, _RET_IP_);

	if (old->bi_bytecount != bytecount) {
		/*
		 * There was a previous value, but the size doesn't match.
		 * This is not allowed.  Drop the old item and return an error.
		 */
		xfs_blobitem_rele(old);
		error = -EINVAL;
		goto delete_bi;
	}

	/*
	 * There was a previous value and the size matches.  Pass the old item
	 * and its active ref to the caller.
	 */
	*bip = old;
	error = -EEXIST;

delete_bi:
	/* Detach @data from bi, then drop both refcounts to free it. */
	bi->bi_data = NULL;
	xfs_blobitem_rele(bi);
	xfs_blobitem_rele(bi);
	return error;
}

/* Set up blob cache and item slabs. */
int __init
xfs_blobcache_insmod(void)
{
	xfs_blobcache_objs = kmem_cache_create("xfs_blobcache",
			sizeof(struct xfs_blobcache), 0, 0, NULL);
	if (!xfs_blobcache_objs)
		return -ENOMEM;

	xfs_blobitem_objs = kmem_cache_create("xfs_blobitem",
			sizeof(struct xfs_blobitem), 0, 0, NULL);
	if (!xfs_blobitem_objs)
		goto out_cache;

	return 0;

out_cache:
	kmem_cache_destroy(xfs_blobcache_objs);
	return -ENOMEM;
}

/* Tear down blob cache and item slabs. */
void
xfs_blobcache_rmmod(void)
{
	kmem_cache_destroy(xfs_blobitem_objs);
	kmem_cache_destroy(xfs_blobcache_objs);
}
