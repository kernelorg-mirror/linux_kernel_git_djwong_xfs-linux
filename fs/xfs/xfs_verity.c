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
#include "xfs_blobcache.h"

static inline struct xfs_blobcache *xfs_verity_get_cache(struct xfs_inode *ip)
{
	/*
	 * The acquire here pairs with the cmpxchg_release in the cache setup
	 * function below.
	 */
	return smp_load_acquire(&ip->i_merkle_blocks);
}


/*
 * Construct the incore buffer cache that we use to cache merkle tree blocks.
 * The caller might have IOLOCK_SHARED, so we must be careful with the i_verity
 * pointer store.
 */
static int
xfs_verity_setup_cache(
	struct xfs_inode	*ip)
{
	struct xfs_blobcache	*bc, *old;

	bc = xfs_blobcache_alloc(ip->i_mount, "merkle", ip->i_ino);
	if (IS_ERR(bc))
		return PTR_ERR(bc);

	/*
	 * We might be called upon to set up the merkle tree block cache while
	 * only holding IOLOCK_SHARED.  Hence we must use cmpxchg to set the
	 * pointer or back off if we lost the race to do so.  The cache only
	 * gets destroyed after the inode has been isolated, so the only race
	 * here is to set the value, not to clear it.
	 *
	 * The cmpxchg_release pairs with the smp_load_acquire above.
	 */
	old = cmpxchg_release(&ip->i_merkle_blocks, NULL, bc);
	if (old)
		xfs_blobcache_free(bc);

	return 0;
}

/*
 * Dump all the cached merkle tree blocks and the cache itself.  Caller must
 * ensure that there are no other threads accessing i_merkle_blocks.
 */
void
__xfs_verity_destroy_cache(
	struct xfs_inode	*ip)
{
	struct xfs_blobcache	*bc = ip->i_merkle_blocks;

	ip->i_merkle_blocks = NULL;
	xfs_blobcache_free(bc);
}

static int
xfs_get_verity_descriptor(
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
xfs_begin_enable_verity(
	struct file	    *filp)
{
	struct inode	    *inode = file_inode(filp);
	struct xfs_inode    *ip = XFS_I(inode);
	int		    error = 0;

	xfs_assert_ilocked(ip, XFS_IOLOCK_EXCL);

	if (IS_DAX(inode))
		return -EINVAL;

	if (xfs_iflags_test_and_set(ip, XFS_IVERITY_CONSTRUCTION))
		return -EBUSY;

	return error;
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
xfs_end_enable_verity(
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
xfs_read_merkle_tree_block(
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
	struct xfs_blobcache		*bc;
	struct xfs_blobitem		*bi;
	xfs_blobitem_key_t		key = block->offset >> req->log_blocksize;
	int				error;

	ASSERT(xfs_blobitem_key_check(block->offset >> req->log_blocksize));

	xfs_fsverity_merkle_key_to_disk(&name, block->offset);

	bc = xfs_verity_get_cache(ip);
	if (!bc) {
		error = xfs_verity_setup_cache(ip);
		if (error)
			return error;

		bc = xfs_verity_get_cache(ip);
	}

	/* Is the block already cached? */
	bi = xfs_blobcache_load(bc, key, block->size);
	if (!IS_ERR_OR_NULL(bi))
		goto out_hit;

	/* Read the block in from disk and try to store it in the cache. */
	xfs_fsverity_merkle_key_to_disk(&name, block->offset);

	error = xfs_attr_get(&args);
	if (error)
		return error;

	if (!args.valuelen)
		return -ENODATA;

	error = xfs_blobcache_store(bc, key, args.value, block->size, &bi);
	switch (error) {
	case -EEXIST:
		/*
		 * We raced with another thread to populate the cache and lost.
		 * Free the attr value buffer but keep going.
		 */
		kvfree(args.value);
		break;
	case 0:
		/*
		 * The merkle tree block is now cached and owns the attr value
		 * buffer.  Keep going.
		 */
		break;
	default:
		/* An error occurred, free the attr value buffer and bail. */
		kvfree(args.value);
		return error;
	}

	/* We might have loaded this in from disk, fsverity must recheck */
	fsverity_invalidate_block(req->inode, block);

out_hit:
	block->kaddr   = (void *)bi->bi_data;
	block->context = bi;

	/*
	 * Prioritize keeping the root-adjacent levels cached if this isn't a
	 * streaming read.
	 */
	if (req->level >= 0)
		xfs_blobitem_set_shrinkref(bi, req->level + 1);

	return 0;
}

static int
xfs_write_merkle_tree_block(
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
xfs_drop_block(
	struct fsverity_blockbuf	*block)
{
	struct xfs_blobitem		*bi = block->context;

	xfs_blobitem_rele(bi);
	block->kaddr = NULL;
	block->context = NULL;
}

const struct fsverity_operations xfs_verity_ops = {
	.begin_enable_verity		= &xfs_begin_enable_verity,
	.end_enable_verity		= &xfs_end_enable_verity,
	.get_verity_descriptor		= &xfs_get_verity_descriptor,
	.read_merkle_tree_block		= &xfs_read_merkle_tree_block,
	.write_merkle_tree_block	= &xfs_write_merkle_tree_block,
	.drop_block			= &xfs_drop_block,
};
