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

/*
 * Make fs-verity invalidate verified status of Merkle tree block
 */
static void
xfs_verity_put_listent(
	struct xfs_attr_list_context	*context,
	int				flags,
	unsigned char			*name,
	int				namelen,
	int				valuelen)
{
	struct fsverity_blockbuf	block = {
		.offset = xfs_fsverity_name_to_block_offset(name),
		.size = valuelen,
	};
	/*
	 * Verity descriptor is smaller than 1024; verity block min size is
	 * 1024. Exclude verity descriptor
	 */
	if (valuelen < 1024)
		return;

	fsverity_invalidate_block(VFS_I(context->dp), &block);
}

/*
 * Iterate over extended attributes in the bp to invalidate Merkle tree blocks
 */
static int
xfs_invalidate_blocks(
	struct xfs_inode	*ip,
	struct xfs_buf		*bp)
{
	struct xfs_attr_list_context context;

	context.dp = ip;
	context.resynch = 0;
	context.buffer = NULL;
	context.bufsize = 0;
	context.firstu = 0;
	context.attr_filter = XFS_ATTR_VERITY;
	context.put_listent = xfs_verity_put_listent;

	return xfs_attr3_leaf_list_int(bp, &context);
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
	struct xfs_inode		*ip = XFS_I(inode);
	struct xfs_fsverity_merkle_key	name;
	int				error = 0;
	struct xfs_da_args		args = {
		.dp			= ip,
		.attr_filter		= XFS_ATTR_VERITY,
		.op_flags		= XFS_DA_OP_BUFFER,
		.namelen		= sizeof(struct xfs_fsverity_merkle_key),
		.valuelen		= (1 << log_blocksize),
	};
	xfs_fsverity_merkle_key_to_disk(&name, pos);
	args.name = (const uint8_t *)&name.merkleoff;

	error = xfs_attr_get(&args);
	if (error)
		goto out;

	if (!args.valuelen)
		return -ENODATA;

	block->kaddr = args.value;
	block->offset = pos;
	block->size = args.valuelen;
	block->context = args.bp;

	/*
	 * Memory barriers are used to force operation ordering of clearing
	 * bitmap in fsverity_invalidate_block() and setting XBF_VERITY_SEEN
	 * flag.
	 *
	 * Multiple threads may execute this code concurrently on the same block.
	 * This is safe because we use memory barriers to ensure that if a
	 * thread sees XBF_VERITY_SEEN, then fsverity bitmap is already up to
	 * date.
	 *
	 * Invalidating block in a bitmap again at worst causes a hash block to
	 * be verified redundantly. That event should be very rare, so it's not
	 * worth using a lock to avoid.
	 */
	if (!(args.bp->b_flags & XBF_VERITY_SEEN)) {
		/*
		 * A read memory barrier is needed here to give ACQUIRE
		 * semantics to the above check.
		 */
		smp_rmb();
		/*
		 * fs-verity is not aware if buffer was evicted from the memory.
		 * Make fs-verity invalidate verfied status of all blocks in the
		 * buffer.
		 *
		 * Single extended attribute can contain multiple Merkle tree
		 * blocks:
		 * - leaf with inline data -> invalidate all blocks in the leaf
		 * - remote value -> invalidate single block
		 *
		 * For example, leaf on 64k system with 4k/1k filesystem will
		 * contain multiple Merkle tree blocks.
		 *
		 * Only remote value buffers would have XBF_DOUBLE_ALLOC flag
		 */
		if (args.bp->b_flags & XBF_DOUBLE_ALLOC)
			fsverity_invalidate_block(inode, block);
		else {
			error = xfs_invalidate_blocks(ip, args.bp);
			if (error)
				goto out;
		}
	}

	/*
	 * A write memory barrier is needed here to give RELEASE
	 * semantics to the below flag.
	 */
	smp_wmb();
	args.bp->b_flags |= XBF_VERITY_SEEN;

	return error;

out:
	kvfree(args.value);
	if (args.bp)
		xfs_buf_rele(args.bp);
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
	struct xfs_buf			*bp;

	ASSERT(block != NULL);
	bp = (struct xfs_buf *)block->context;
	ASSERT(bp->b_flags & XBF_VERITY_SEEN);

	xfs_buf_rele(bp);

	kunmap_local(block->kaddr);
}

const struct fsverity_operations xfs_verity_ops = {
	.begin_enable_verity		= &xfs_begin_enable_verity,
	.end_enable_verity		= &xfs_end_enable_verity,
	.get_verity_descriptor		= &xfs_get_verity_descriptor,
	.read_merkle_tree_block		= &xfs_read_merkle_tree_block,
	.write_merkle_tree_block	= &xfs_write_merkle_tree_block,
	.drop_block			= &xfs_drop_block,
};
