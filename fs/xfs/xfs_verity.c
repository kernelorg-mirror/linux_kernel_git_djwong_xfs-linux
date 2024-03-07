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

/*
 * Merkle Tree Block Cache
 * =======================
 *
 */
struct xfs_merkle_blob {
	/* refcount of this item; the cache holds its own ref */
	refcount_t		mk_refcount;

	/* number of times the shrinker should ignore this item */
	atomic_t		mk_shrinkref;

	/* blob data, must be last! */
	unsigned char		mk_data[];
};

/* Size of a merkle tree cache block */
static inline size_t sizeof_merkle_blob(unsigned int blocksize)
{
	return struct_size_t(struct xfs_merkle_blob, mk_data, blocksize);
}

/*
 * Allocate a merkle tree blob object to prepare for reading a merkle tree
 * object from disk.
 */
static inline struct xfs_merkle_blob *
xfs_merkle_blob_alloc(
	unsigned int		blocksize)
{
	struct xfs_merkle_blob	*mk;

	mk = kvzalloc(sizeof_merkle_blob(blocksize), GFP_KERNEL);
	if (!mk)
		return NULL;

	/* Caller owns this refcount. */
	refcount_set(&mk->mk_refcount, 1);
	return mk;
}

/* Free a merkle tree blob. */
static inline void
xfs_merkle_blob_rele(
	struct xfs_merkle_blob	*mk)
{
	if (refcount_dec_and_test(&mk->mk_refcount))
		kvfree(mk);
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
	struct xfs_merkle_blob	*mk;

	xas_lock(&xas);
	xas_for_each(&xas, mk, ULONG_MAX) {
		ASSERT(refcount_read(&mk->mk_refcount) == 1);

		trace_xfs_verity_cache_drop(ip, xas.xa_index, _RET_IP_);

		xas_store(&xas, NULL);
		xfs_merkle_blob_rele(mk);
	}
	xas_unlock(&xas);
	xfs_inode_clear_verity_tag(ip);
}

/* Destroy the merkle tree block cache */
void
xfs_verity_cache_destroy(
	struct xfs_inode	*ip)
{
	ASSERT(xa_empty(&ip->i_merkle_blocks));

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
		 (mk && !refcount_inc_not_zero(&mk->mk_refcount)));
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
	struct xfs_merkle_blob	*old;

	trace_xfs_verity_cache_store(ip, key, _RET_IP_);

	/*
	 * Either replace a NULL entry with mk, or take an active ref to
	 * whatever's currently there.
	 */
	xa_lock(&ip->i_merkle_blocks);
	do {
		old = __xa_cmpxchg(&ip->i_merkle_blocks, key, NULL, mk,
				GFP_KERNEL);
	} while (old && !refcount_inc_not_zero(&old->mk_refcount));
	xa_unlock(&ip->i_merkle_blocks);

	if (old == NULL) {
		/*
		 * There was no previous value.  @mk is now live in the cache.
		 * Bump the active refcount to transfer ownership to the cache
		 * and return @mk to the caller.
		 */
		refcount_inc(&mk->mk_refcount);
		return mk;
	}

	/*
	 * We obtained an active reference to a previous value in the cache.
	 * Return it to the caller.
	 */
	return old;
}

/* Reclaim inactive merkle tree blocks that have run out of second chances. */
unsigned long
xfs_verity_cache_shrink_scan(
	struct xfs_inode	*ip,
	unsigned long		nr_to_scan)
{
	XA_STATE(xas, &ip->i_merkle_blocks, 0);
	struct xfs_merkle_blob	*mk;
	unsigned long		freed = 0;

	trace_xfs_verity_cache_shrink_scan(ip, nr_to_scan, _RET_IP_);

	xas_lock(&xas);
	xas_for_each(&xas, mk, ULONG_MAX) {
		/* Retain if there are active references */
		if (refcount_read(&mk->mk_refcount) > 1)
			continue;

		/* Ignore if the item still has lru refcount */
		if (atomic_add_unless(&mk->mk_shrinkref, -1, 0))
			continue;

		trace_xfs_verity_cache_reclaim(ip, xas.xa_index, _RET_IP_);

		freed++;
		xas_store(&xas, NULL);
		xfs_merkle_blob_rele(mk);

		if (freed >= nr_to_scan)
			break;
	}
	xas_unlock(&xas);

	/*
	 * Try to clear the verity tree tag if we reclaimed all the cached
	 * blocks.  On the flag setting side, we should have IOLOCK_SHARED.
	 */
	xfs_ilock(ip, XFS_IOLOCK_EXCL);
	if (xa_empty(&ip->i_merkle_blocks))
		xfs_inode_clear_verity_tag(ip);
	xfs_iunlock(ip, XFS_IOLOCK_EXCL);

	trace_xfs_verity_cache_shrink_freed(ip, freed, _RET_IP_);
	return freed;
}

/* Count the number of inactive merkle tree blocks that could be reclaimed. */
unsigned long
xfs_verity_cache_shrink_count(
	struct xfs_inode	*ip)
{
	XA_STATE(xas, &ip->i_merkle_blocks, 0);
	struct xfs_merkle_blob	*mk;
	unsigned long		count = 0;

	rcu_read_lock();
	xas_for_each(&xas, mk, ULONG_MAX) {
		if (refcount_read(&mk->mk_refcount) == 1)
			count++;
	}
	rcu_read_unlock();

	trace_xfs_verity_cache_shrink_count(ip, count, _RET_IP_);
	return count;
}

static inline void
xfs_fsverity_merkle_key_to_disk(
	struct xfs_fsverity_merkle_key	*key,
	u64				offset)
{
	key->merkleoff = cpu_to_be64(offset);
}

static inline u64
xfs_fsverity_merkle_key_from_disk(
	void				*attr_name)
{
	struct xfs_fsverity_merkle_key	*key = attr_name;

	return be64_to_cpu(key->merkleoff);
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

static int
xfs_verity_begin_enable(
	struct file		*filp,
	u64			merkle_tree_size,
	unsigned int		tree_blocksize)
{
	struct inode		*inode = file_inode(filp);
	struct xfs_inode	*ip = XFS_I(inode);
	int			error = 0;

	xfs_assert_ilocked(ip, XFS_IOLOCK_EXCL);

	if (IS_DAX(inode))
		return -EINVAL;

	if (xfs_iflags_test_and_set(ip, XFS_IVERITY_CONSTRUCTION))
		return -EBUSY;

	return error;
}

static int
xfs_drop_merkle_tree(
	struct xfs_inode		*ip,
	u64				merkle_tree_size,
	unsigned int			tree_blocksize)
{
	struct xfs_fsverity_merkle_key	name;
	int				error = 0;
	u64				offset = 0;
	struct xfs_da_args		args = {
		.dp			= ip,
		.whichfork		= XFS_ATTR_FORK,
		.attr_filter		= XFS_ATTR_VERITY,
		.op_flags		= XFS_DA_OP_REMOVE,
		.name			= (const uint8_t *)&name,
		.namelen		= sizeof(struct xfs_fsverity_merkle_key),
		/* NULL value make xfs_attr_set remove the attr */
		.value			= NULL,
	};

	if (!merkle_tree_size)
		return 0;

	for (offset = 0; offset < merkle_tree_size; offset += tree_blocksize) {
		xfs_fsverity_merkle_key_to_disk(&name, offset);
		error = xfs_attr_set(&args);
		if (error)
			return error;
	}

	args.name = (const uint8_t *)XFS_VERITY_DESCRIPTOR_NAME;
	args.namelen = XFS_VERITY_DESCRIPTOR_NAME_LEN;
	error = xfs_attr_set(&args);

	return error;
}

static int
xfs_verity_enable_end(
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
		.attr_flags	= XATTR_CREATE,
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
	if (error)
		WARN_ON_ONCE(xfs_drop_merkle_tree(ip, merkle_tree_size,
						  tree_blocksize));

	xfs_iflags_clear(ip, XFS_IVERITY_CONSTRUCTION);
	return error;
}

static int
xfs_verity_read_merkle_tree_block(
	const struct fsverity_readmerkle *req,
	struct fsverity_blockbuf	*block)
{
	struct xfs_inode		*ip = XFS_I(req->inode);
	struct xfs_fsverity_merkle_key	name;
	struct xfs_da_args		args = {
		.dp			= ip,
		.attr_filter		= XFS_ATTR_VERITY,
		.name			= (const uint8_t *)&name,
		.namelen		= sizeof(struct xfs_fsverity_merkle_key),
		.valuelen		= block->size,
	};
	struct xfs_merkle_blob		*mk, *new_mk;
	unsigned long			key = block->offset >> req->log_blocksize;
	int				error;

	ASSERT(block->offset >> req->log_blocksize <= ULONG_MAX);

	xfs_fsverity_merkle_key_to_disk(&name, block->offset);

	/* Is the block already cached? */
	mk = xfs_verity_cache_load(ip, key);
	if (mk)
		goto out_hit;

	new_mk = xfs_merkle_blob_alloc(block->size);
	if (!new_mk)
		return -ENOMEM;
	args.value = new_mk->mk_data;

	/* Read the block in from disk and try to store it in the cache. */
	xfs_fsverity_merkle_key_to_disk(&name, block->offset);

	error = xfs_attr_get(&args);
	if (error)
		goto out_new_mk;

	if (!args.valuelen) {
		error = -ENODATA;
		goto out_new_mk;
	}

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

	/* We might have loaded this in from disk, fsverity must recheck */
	fsverity_invalidate_block(req->inode, block);

out_hit:
	block->kaddr   = (void *)mk->mk_data;
	block->context = mk;

	/*
	 * Prioritize keeping the root-adjacent levels cached if this isn't a
	 * streaming read.
	 */
	if (req->level >= 0)
		atomic_set(&mk->mk_shrinkref, req->level + 1);
	return 0;

out_new_mk:
	xfs_merkle_blob_rele(new_mk);
	return error;
}

static int
xfs_verity_write_merkle_tree_block(
	struct inode		*inode,
	const void		*buf,
	u64			pos,
	unsigned int		size)
{
	struct xfs_inode	*ip = XFS_I(inode);
	struct xfs_fsverity_merkle_key	name;
	struct xfs_da_args	args = {
		.dp		= ip,
		.whichfork	= XFS_ATTR_FORK,
		.attr_filter	= XFS_ATTR_VERITY,
		.attr_flags	= XATTR_CREATE,
		.namelen	= sizeof(struct xfs_fsverity_merkle_key),
		.value		= (void *)buf,
		.valuelen	= size,
	};

	xfs_fsverity_merkle_key_to_disk(&name, pos);
	args.name = (const uint8_t *)&name.merkleoff;

	return xfs_attr_set(&args);
}

static void
xfs_verity_drop_merkle_tree_block(
	struct fsverity_blockbuf	*block)
{
	struct xfs_merkle_blob		*mk = block->context;

	xfs_merkle_blob_rele(mk);
	block->kaddr = NULL;
	block->context = NULL;
}

const struct fsverity_operations xfs_verity_ops = {
	.begin_enable_verity		= xfs_verity_begin_enable,
	.end_enable_verity		= xfs_verity_enable_end,
	.get_verity_descriptor		= xfs_verity_get_descriptor,
	.read_merkle_tree_block		= xfs_verity_read_merkle_tree_block,
	.write_merkle_tree_block	= xfs_verity_write_merkle_tree_block,
	.drop_block			= xfs_verity_drop_merkle_tree_block,
};
