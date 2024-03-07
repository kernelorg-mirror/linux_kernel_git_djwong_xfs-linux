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
#include "xfs_buf_mem.h"

/*
 * Construct the incore buffer cache that we use to cache merkle tree blocks.
 * The caller might have IOLOCK_SHARED, so we must be careful with the i_verity
 * pointer store.
 */
static int
xfs_verity_setup_cache(
	struct xfs_inode	*ip)
{
	struct xfs_buftarg	*cache, *old;
	int			error = 0;

	error = xblobc_alloc(ip->i_mount, ip->i_ino, "merkle", &cache);
	if (error)
		return error;

	/*
	 * We might be called upon to set up the merkle tree block cache while
	 * only holding IOLOCK_SHARED.  Hence we must use cmpxchg to set the
	 * pointer or back off if we lost the race to do so.  The cache only
	 * gets destroyed after the inode has been isolated, so the only race
	 * here is to set the value.
	 */
	old = cmpxchg(&ip->i_merkle_blocks, NULL, cache);
	if (old)
		xblobc_free(cache);

	return 0;
}

/* Dump all the cached merkle tree blocks and the cache itself. */
void
__xfs_verity_destroy_cache(
	struct xfs_inode	*ip)
{
	struct xfs_buftarg	*cache = ip->i_merkle_blocks;

	ip->i_merkle_blocks = NULL;
	xblobc_free(cache);
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
		.namelen		= sizeof(struct xfs_fsverity_merkle_key),
		/* NULL value make xfs_attr_set remove the attr */
		.value			= NULL,
	};

	if (!merkle_tree_size)
		return 0;

	args.name = (const uint8_t *)&name.merkleoff;
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
	struct inode			*inode,
	u64				pos,
	struct fsverity_blockbuf	*block,
	unsigned int			log_blocksize,
	u64				ra_bytes)
{
	struct xfs_fsverity_merkle_key	name;
	struct xfs_inode		*ip = XFS_I(inode);
	struct xfs_da_args		args = {
		.dp			= ip,
		.attr_filter		= XFS_ATTR_VERITY,
		.namelen		= sizeof(struct xfs_fsverity_merkle_key),
		.valuelen		= (1 << log_blocksize),
	};
	struct xfs_buf			*bp;
	int				error = 0;

	if (!xfs_verity_has_cache(ip)) {
		error = xfs_verity_setup_cache(ip);
		if (error)
			return error;
	}

	error = xblobc_peek(ip->i_merkle_blocks, pos, args.valuelen, &bp);
	switch (error) {
	case -ENODATA:
		/* not cached */
		break;
	case 0:
		/* cache still has the block; feed it back to fsverity. */
		goto out_hit;
	default:
		return error;
	}

	xfs_fsverity_merkle_key_to_disk(&name, pos);
	args.name = (const uint8_t *)&name.merkleoff;

	error = xfs_attr_get(&args);
	if (error)
		return error;

	if (!args.valuelen)
		return -ENODATA;

	error = xblobc_get(ip->i_merkle_blocks, pos, args.valuelen, &bp);
	if (error)
		goto out_valuebuf;

	xblobc_store(bp, args.value, args.valuelen);

	/* loaded this in from disk, tell fsverity to recheck */
	fsverity_invalidate_block(inode, block);

out_hit:
	block->kaddr   = bp->b_addr;
	block->offset  = pos;
	block->size    = args.valuelen;
	block->context = bp;
	return 0;

out_valuebuf:
	kvfree(args.value);
	return error;
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
	struct xfs_buf			*bp = block->context;

	xfs_buf_relse(bp);
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
