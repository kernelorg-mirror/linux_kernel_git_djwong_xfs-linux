/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Red Hat, Inc.
 */
#include "xfs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_log_format.h"
#include "xfs_attr.h"
#include "xfs_verity.h"
#include "xfs_bmap_util.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_attr_leaf.h"
#include "xfs_trace.h"
#include "xfs_icache.h"
#include <linux/fsverity.h>

/*
 * Merkle Tree Block Cache
 * =======================
 *
 * fsverity requires that the filesystem implement caching of ondisk merkle
 * tree blocks.  XFS stores merkle tree blocks in the extended attribute data,
 * which makes it important to keep copies in memory for as long as possible.
 * This is performed by allocating the data blob structure defined below,
 * passing the data portion of the blob to xfs_attr_get, and later adding the
 * data blob to an xarray embedded in the xfs_inode structure.
 *
 * The xarray structure indexes merkle tree blocks by the offset given to us by
 * fsverity, which drastically reduces lookups.  First, it eliminating the need
 * to walk the xattr structure to find the remote block containing the merkle
 * tree block.  Second, access to each block in the xattr structure requires a
 * lookup in the incore extent btree.
 */
struct xfs_merkle_blob {
	/* refcount of this item; the cache holds its own ref */
	refcount_t		refcount;

	/* number of times the shrinker should ignore this item */
	atomic_t		shrinkref;

	unsigned long		flags;

	/* Pointer to the merkle tree block, which is power-of-2 sized */
	void			*data;
};

#define XFS_MERKLE_BLOB_VERIFIED_BIT	(0) /* fsverity validated this */

/*
 * Allocate a merkle tree blob object to prepare for reading a merkle tree
 * object from disk.
 */
static inline struct xfs_merkle_blob *
xfs_merkle_blob_alloc(
	unsigned int		blocksize)
{
	struct xfs_merkle_blob	*mk;

	mk = kmalloc(sizeof(struct xfs_merkle_blob), GFP_KERNEL);
	if (!mk)
		return NULL;

	mk->data = kvzalloc(blocksize, GFP_KERNEL);
	if (!mk->data) {
		kfree(mk);
		return NULL;
	}

	/* Caller owns this refcount. */
	refcount_set(&mk->refcount, 1);
	atomic_set(&mk->shrinkref, 0);
	mk->flags = 0;
	return mk;
}

/* Free a merkle tree blob. */
static inline void
xfs_merkle_blob_rele(
	struct xfs_merkle_blob	*mk)
{
	if (refcount_dec_and_test(&mk->refcount)) {
		kvfree(mk->data);
		kfree(mk);
	}
}

/* Initialize the merkle tree block cache */
void
xfs_verity_cache_init(
	struct xfs_inode	*ip)
{
	xa_init(&ip->i_merkle_blocks);
}

/*
 * Drop all the merkle tree blocks out of the cache.  Caller must ensure that
 * there are no active references to cache items.
 */
void
xfs_verity_cache_drop(
	struct xfs_inode	*ip)
{
	XA_STATE(xas, &ip->i_merkle_blocks, 0);
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_merkle_blob	*mk;
	unsigned long		flags;
	s64			freed = 0;

	xas_lock_irqsave(&xas, flags);
	xas_for_each(&xas, mk, ULONG_MAX) {
		ASSERT(refcount_read(&mk->refcount) == 1);

		trace_xfs_verity_cache_drop(ip, xas.xa_index, _RET_IP_);

		freed++;
		xas_store(&xas, NULL);
		xfs_merkle_blob_rele(mk);
	}
	percpu_counter_sub(&mp->m_verity_blocks, freed);
	xas_unlock_irqrestore(&xas, flags);
	xfs_inode_clear_verity_tag(ip);
}

/* Destroy the merkle tree block cache */
void
xfs_verity_cache_destroy(
	struct xfs_inode	*ip)
{
	ASSERT(xa_empty(&ip->i_merkle_blocks));

	/*
	 * xa_destroy calls xas_lock from rcu freeing softirq context, so
	 * we must use xa*_lock_irqsave.
	 */
	xa_destroy(&ip->i_merkle_blocks);
}

/* Return a cached merkle tree block, or NULL. */
static struct xfs_merkle_blob *
xfs_verity_cache_load(
	struct xfs_inode	*ip,
	unsigned long		key)
{
	XA_STATE(xas, &ip->i_merkle_blocks, key);
	struct xfs_merkle_blob	*mk;

	/* Look up the cached item and try to get an active ref. */
	rcu_read_lock();
	do {
		mk = xas_load(&xas);
		if (xa_is_zero(mk))
			mk = NULL;
	} while (xas_retry(&xas, mk) ||
		 (mk && !refcount_inc_not_zero(&mk->refcount)));
	rcu_read_unlock();

	if (!mk)
		return NULL;

	trace_xfs_verity_cache_load(ip, key, _RET_IP_);
	return mk;
}

/*
 * Try to store a merkle tree block in the cache with the given key.
 *
 * If the merkle tree block is not already in the cache, the given block @mk
 * will be added to the cache and returned.  The caller retains its active
 * reference to @mk.
 *
 * If there was already a merkle block in the cache, it will be returned to
 * the caller with an active reference.  @mk will be untouched.
 */
static struct xfs_merkle_blob *
xfs_verity_cache_store(
	struct xfs_inode	*ip,
	unsigned long		key,
	struct xfs_merkle_blob	*mk)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_merkle_blob	*old;
	unsigned long		flags;

	trace_xfs_verity_cache_store(ip, key, _RET_IP_);

	/*
	 * Either replace a NULL entry with mk, or take an active ref to
	 * whatever's currently there.
	 */
	xa_lock_irqsave(&ip->i_merkle_blocks, flags);
	do {
		old = __xa_cmpxchg(&ip->i_merkle_blocks, key, NULL, mk,
				GFP_KERNEL);
	} while (old && !refcount_inc_not_zero(&old->refcount));
	if (!old)
		percpu_counter_add(&mp->m_verity_blocks, 1);
	xa_unlock_irqrestore(&ip->i_merkle_blocks, flags);

	if (old == NULL) {
		/*
		 * There was no previous value.  @mk is now live in the cache.
		 * Bump the active refcount to transfer ownership to the cache
		 * and return @mk to the caller.
		 */
		refcount_inc(&mk->refcount);
		return mk;
	}

	/*
	 * We obtained an active reference to a previous value in the cache.
	 * Return it to the caller.
	 */
	return old;
}

/* Count the merkle tree blocks that we might be able to reclaim. */
static unsigned long
xfs_verity_shrinker_count(
	struct shrinker		*shrink,
	struct shrink_control	*sc)
{
	struct xfs_mount	*mp = shrink->private_data;
	s64			count;

	if (!xfs_has_verity(mp))
		return SHRINK_EMPTY;

	count = percpu_counter_sum_positive(&mp->m_verity_blocks);

	trace_xfs_verity_shrinker_count(mp, count, _RET_IP_);
	return min_t(s64, ULONG_MAX, count);
}

struct xfs_verity_scan {
	struct xfs_icwalk	icw;
	struct shrink_control	*sc;

	unsigned long		scanned;
	unsigned long		freed;
};

/* Reclaim inactive merkle tree blocks that have run out of second chances. */
static void
xfs_verity_cache_reclaim(
	struct xfs_inode	*ip,
	struct xfs_verity_scan	*vs)
{
	XA_STATE(xas, &ip->i_merkle_blocks, 0);
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_merkle_blob	*mk;
	unsigned long		flags;
	s64			freed = 0;

	xas_lock_irqsave(&xas, flags);
	xas_for_each(&xas, mk, ULONG_MAX) {
		/*
		 * Tell the shrinker that we scanned this merkle tree block,
		 * even if we don't remove it.
		 */
		vs->scanned++;
		if (vs->sc->nr_to_scan-- == 0)
			break;

		/* Retain if there are active references */
		if (refcount_read(&mk->refcount) > 1)
			continue;

		/* Ignore if the item still has lru refcount */
		if (atomic_add_unless(&mk->shrinkref, -1, 0))
			continue;

		trace_xfs_verity_cache_reclaim(ip, xas.xa_index, _RET_IP_);

		freed++;
		xas_store(&xas, NULL);
		xfs_merkle_blob_rele(mk);
	}
	percpu_counter_sub(&mp->m_verity_blocks, freed);
	xas_unlock_irqrestore(&xas, flags);

	/*
	 * Try to clear the verity tree tag if we reclaimed all the cached
	 * blocks.  On the flag setting side, we should have IOLOCK_SHARED.
	 */
	xfs_ilock(ip, XFS_IOLOCK_EXCL);
	if (xa_empty(&ip->i_merkle_blocks))
		xfs_inode_clear_verity_tag(ip);
	xfs_iunlock(ip, XFS_IOLOCK_EXCL);

	vs->freed += freed;
}

/* Scan an inode as part of a verity scan. */
int
xfs_verity_scan_inode(
	struct xfs_inode	*ip,
	struct xfs_icwalk	*icw)
{
	struct xfs_verity_scan	*vs;

	vs = container_of(icw, struct xfs_verity_scan, icw);

	if (vs->sc->nr_to_scan > 0)
		xfs_verity_cache_reclaim(ip, vs);

	if (vs->sc->nr_to_scan == 0)
		xfs_icwalk_verity_stop(icw);

	xfs_irele(ip);
	return 0;
}

/* Actually try to reclaim merkle tree blocks. */
static unsigned long
xfs_verity_shrinker_scan(
	struct shrinker		*shrink,
	struct shrink_control	*sc)
{
	struct xfs_verity_scan	vs = {
		.sc		= sc,
	};
	struct xfs_mount	*mp = shrink->private_data;
	int			error;

	if (!xfs_has_verity(mp))
		return SHRINK_STOP;

	error = xfs_icwalk_verity(mp, &vs.icw);
	if (error)
		xfs_alert(mp, "%s: verity scan failed, error %d", __func__,
				error);

	trace_xfs_verity_shrinker_scan(mp, vs.scanned, vs.freed, _RET_IP_);
	return vs.freed;
}

/* Register a shrinker so we can release cached merkle tree blocks. */
int
xfs_verity_register_shrinker(
	struct xfs_mount	*mp)
{
	int			error;

	if (!xfs_has_verity(mp))
		return 0;

	error = percpu_counter_init(&mp->m_verity_blocks, 0, GFP_KERNEL);
	if (error)
		return error;

	mp->m_verity_shrinker = shrinker_alloc(0, "xfs-verity:%s",
			mp->m_super->s_id);
	if (!mp->m_verity_shrinker) {
		percpu_counter_destroy(&mp->m_verity_blocks);
		return -ENOMEM;
	}

	mp->m_verity_shrinker->count_objects = xfs_verity_shrinker_count;
	mp->m_verity_shrinker->scan_objects = xfs_verity_shrinker_scan;
	mp->m_verity_shrinker->seeks = 0;
	mp->m_verity_shrinker->private_data = mp;

	shrinker_register(mp->m_verity_shrinker);

	return 0;
}

/* Unregister the merkle tree block shrinker. */
void
xfs_verity_unregister_shrinker(struct xfs_mount *mp)
{
	if (!xfs_has_verity(mp))
		return;

	ASSERT(percpu_counter_sum(&mp->m_verity_blocks) == 0);

	shrinker_free(mp->m_verity_shrinker);
	percpu_counter_destroy(&mp->m_verity_blocks);
}

static int
xfs_verity_get_descriptor(
	struct inode		*inode,
	void			*buf,
	size_t			buf_size)
{
	struct xfs_inode	*ip = XFS_I(inode);
	int			error = 0;
	struct xfs_da_args	args = {
		.dp		= ip,
		.attr_filter	= XFS_ATTR_VERITY,
		.name		= (const uint8_t *)XFS_VERITY_DESCRIPTOR_NAME,
		.namelen	= XFS_VERITY_DESCRIPTOR_NAME_LEN,
		.value		= buf,
		.valuelen	= buf_size,
	};

	/*
	 * The fact that (returned attribute size) == (provided buf_size) is
	 * checked by xfs_attr_copy_value() (returns -ERANGE)
	 */
	error = xfs_attr_get(&args);
	if (error)
		return error;

	return args.valuelen;
}

/*
 * Clear out old fsverity metadata before we start building a new one.  This
 * could happen if, say, we crashed while building fsverity data.
 */
static int
xfs_verity_drop_old_metadata(
	struct xfs_inode		*ip,
	u64				new_tree_size,
	unsigned int			tree_blocksize)
{
	struct xfs_verity_merkle_key	name;
	struct xfs_da_args		args = {
		.dp			= ip,
		.whichfork		= XFS_ATTR_FORK,
		.attr_filter		= XFS_ATTR_VERITY,
		.op_flags		= XFS_DA_OP_REMOVE,
		.name			= (const uint8_t *)&name,
		.namelen		= sizeof(struct xfs_verity_merkle_key),
		/* NULL value make xfs_attr_set remove the attr */
		.value			= NULL,
	};
	u64				offset;
	int				error = 0;

	/*
	 * Delete as many merkle tree blocks in increasing blkno order until we
	 * don't find any more.  That ought to be good enough for avoiding
	 * dead bloat without excessive runtime.
	 */
	for (offset = new_tree_size; !error; offset += tree_blocksize) {
		xfs_verity_merkle_key_to_disk(&name, offset);
		error = xfs_attr_set(&args);
	}
	if (error == -ENOATTR)
		return 0;
	return error;
}

static int
xfs_verity_begin_enable(
	struct file		*filp,
	u64			merkle_tree_size,
	unsigned int		tree_blocksize)
{
	struct inode		*inode = file_inode(filp);
	struct xfs_inode	*ip = XFS_I(inode);

	xfs_assert_ilocked(ip, XFS_IOLOCK_EXCL);

	if (IS_DAX(inode))
		return -EINVAL;

	if (xfs_iflags_test_and_set(ip, XFS_VERITY_CONSTRUCTION))
		return -EBUSY;

	return xfs_verity_drop_old_metadata(ip, merkle_tree_size,
			tree_blocksize);
}

/* Try to remove all the fsverity metadata after a failed enablement. */
static int
xfs_verity_drop_incomplete_tree(
	struct xfs_inode		*ip,
	u64				merkle_tree_size,
	unsigned int			tree_blocksize)
{
	struct xfs_verity_merkle_key	name;
	struct xfs_da_args		args = {
		.dp			= ip,
		.whichfork		= XFS_ATTR_FORK,
		.attr_filter		= XFS_ATTR_VERITY,
		.op_flags		= XFS_DA_OP_REMOVE,
		.name			= (const uint8_t *)&name,
		.namelen		= sizeof(struct xfs_verity_merkle_key),
		/* NULL value make xfs_attr_set remove the attr */
		.value			= NULL,
	};
	u64				offset;
	int				error;

	if (!merkle_tree_size)
		return 0;

	for (offset = 0; offset < merkle_tree_size; offset += tree_blocksize) {
		xfs_verity_merkle_key_to_disk(&name, offset);
		error = xfs_attr_set(&args);
		if (error == -ENOATTR)
			error = 0;
		if (error)
			return error;
	}

	args.name = (const uint8_t *)XFS_VERITY_DESCRIPTOR_NAME;
	args.namelen = XFS_VERITY_DESCRIPTOR_NAME_LEN;
	error = xfs_attr_set(&args);
	if (error == -ENOATTR)
		return 0;
	return error;
}

static int
xfs_verity_end_enable(
	struct file		*filp,
	const void		*desc,
	size_t			desc_size,
	u64			merkle_tree_size,
	unsigned int		tree_blocksize)
{
	struct inode		*inode = file_inode(filp);
	struct xfs_inode	*ip = XFS_I(inode);
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	struct xfs_da_args	args = {
		.dp		= ip,
		.whichfork	= XFS_ATTR_FORK,
		.attr_filter	= XFS_ATTR_VERITY,
		.name		= (const uint8_t *)XFS_VERITY_DESCRIPTOR_NAME,
		.namelen	= XFS_VERITY_DESCRIPTOR_NAME_LEN,
		.value		= (void *)desc,
		.valuelen	= desc_size,
	};
	int			error = 0;

	xfs_assert_ilocked(ip, XFS_IOLOCK_EXCL);

	/* fs-verity failed, just cleanup */
	if (desc == NULL)
		goto out;

	error = xfs_attr_set(&args);
	if (error)
		goto out;

	/* Set fsverity inode flag */
	error = xfs_trans_alloc_inode(ip, &M_RES(mp)->tr_ichange,
			0, 0, false, &tp);
	if (error)
		goto out;

	/*
	 * Ensure that we've persisted the verity information before we enable
	 * it on the inode and tell the caller we have sealed the inode.
	 */
	ip->i_diflags2 |= XFS_DIFLAG2_VERITY;

	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	xfs_trans_set_sync(tp);

	error = xfs_trans_commit(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);

	if (!error)
		inode->i_flags |= S_VERITY;

out:
	if (error) {
		int	error2;

		error2 = xfs_verity_drop_incomplete_tree(ip, merkle_tree_size,
				tree_blocksize);
		if (error2)
			xfs_alert(ip->i_mount,
 "ino 0x%llx failed to clean up new fsverity metadata, err %d",
					ip->i_ino, error2);
	}

	xfs_iflags_clear(ip, XFS_VERITY_CONSTRUCTION);
	return error;
}

static int
xfs_verity_read_merkle(
	const struct fsverity_readmerkle *req,
	struct fsverity_blockbuf	*block)
{
	struct xfs_inode		*ip = XFS_I(req->inode);
	struct xfs_verity_merkle_key	name;
	struct xfs_da_args		args = {
		.dp			= ip,
		.attr_filter		= XFS_ATTR_VERITY,
		.name			= (const uint8_t *)&name,
		.namelen		= sizeof(struct xfs_verity_merkle_key),
		.valuelen		= block->size,
	};
	struct xfs_merkle_blob		*mk, *new_mk;
	unsigned long			key = block->offset >> req->log_blocksize;
	int				error;

	ASSERT(block->offset >> req->log_blocksize <= ULONG_MAX);

	xfs_verity_merkle_key_to_disk(&name, block->offset);

	/* Is the block already cached? */
	mk = xfs_verity_cache_load(ip, key);
	if (mk)
		goto out_hit;

	new_mk = xfs_merkle_blob_alloc(block->size);
	if (!new_mk)
		return -ENOMEM;
	args.value = new_mk->data;

	/* Read the block in from disk and try to store it in the cache. */
	xfs_verity_merkle_key_to_disk(&name, block->offset);

	error = xfs_attr_get(&args);
	if (error)
		goto out_new_mk;

	mk = xfs_verity_cache_store(ip, key, new_mk);
	if (mk != new_mk) {
		/*
		 * We raced with another thread to populate the cache and lost.
		 * Free the new cache blob and continue with the existing one.
		 */
		xfs_merkle_blob_rele(new_mk);
	} else {
		/*
		 * We added this merkle tree block to the cache; tag the inode
		 * so that reclaim will scan this inode.  The caller holds
		 * IOLOCK_SHARED this will not race with the shrinker.
		 */
		xfs_inode_set_verity_tag(ip);
	}

out_hit:
	block->kaddr   = (void *)mk->data;
	block->context = mk;
	block->verified = test_bit(XFS_MERKLE_BLOB_VERIFIED_BIT, &mk->flags);

	/*
	 * Prioritize keeping the root-adjacent levels cached if this isn't a
	 * streaming read.
	 */
	if (req->level >= 0)
		atomic_set(&mk->shrinkref, req->level + 1);

	return 0;

out_new_mk:
	xfs_merkle_blob_rele(new_mk);
	return error;
}

static int
xfs_verity_write_merkle(
	const struct fsverity_writemerkle *req,
	const void			*buf,
	u64				pos,
	unsigned int			size)
{
	struct inode			*inode = req->inode;
	struct xfs_inode		*ip = XFS_I(inode);
	struct xfs_verity_merkle_key	name;
	struct xfs_da_args		args = {
		.dp			= ip,
		.whichfork		= XFS_ATTR_FORK,
		.attr_filter		= XFS_ATTR_VERITY,
		.name			= (const uint8_t *)&name,
		.namelen		= sizeof(struct xfs_verity_merkle_key),
		.value			= (void *)buf,
		.valuelen		= size,
	};
	const char			*p = buf + size - 1;

	/* Don't store trailing zeroes. */
	while (p >= (const char *)buf && *p == 0)
		p--;
	args.valuelen = p - (const char *)buf + 1;

	xfs_verity_merkle_key_to_disk(&name, pos);
	return xfs_attr_set(&args);
}

static void
xfs_verity_drop_merkle(
	struct fsverity_blockbuf	*block)
{
	struct xfs_merkle_blob		*mk = block->context;

	if (block->verified)
		set_bit(XFS_MERKLE_BLOB_VERIFIED_BIT, &mk->flags);
	xfs_merkle_blob_rele(mk);
	block->kaddr = NULL;
	block->context = NULL;
}

const struct fsverity_operations xfs_verity_ops = {
	.begin_enable_verity		= xfs_verity_begin_enable,
	.end_enable_verity		= xfs_verity_end_enable,
	.get_verity_descriptor		= xfs_verity_get_descriptor,
	.read_merkle_tree_block		= xfs_verity_read_merkle,
	.write_merkle_tree_block	= xfs_verity_write_merkle,
	.drop_merkle_tree_block		= xfs_verity_drop_merkle,
};
