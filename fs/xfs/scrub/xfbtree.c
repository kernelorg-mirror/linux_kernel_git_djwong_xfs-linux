/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_trans.h"
#include "xfs_btree.h"
#include "xfs_btree_mem.h"
#include "xfs_error.h"
#include "scrub/xfbtree.h"

/* btree ops functions for in-memory btrees. */

void
xfs_btree_mem_set_root(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr,
	int				inc)
{
	struct xfs_buf			*head_bp = cur->bc_mem.head_bp;
	struct xfs_btree_mem_head	*mhead = head_bp->b_addr;

	ASSERT(cur->bc_flags & XFS_BTREE_IN_MEMORY);

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS) {
		mhead->mh_root = ptr->l;
	} else {
		xfs_fileoff_t		root = be32_to_cpu(ptr->s);

		mhead->mh_root = cpu_to_be64(root);
	}
	be32_add_cpu(&mhead->mh_nlevels, inc);
	xfs_trans_log_buf(cur->bc_tp, head_bp, 0, sizeof(*mhead) - 1);
}

void
xfs_btree_mem_init_ptr_from_cur(
	struct xfs_btree_cur		*cur,
	union xfs_btree_ptr		*ptr)
{
	struct xfs_buf			*head_bp = cur->bc_mem.head_bp;
	struct xfs_btree_mem_head	*mhead = head_bp->b_addr;

	ASSERT(cur->bc_flags & XFS_BTREE_IN_MEMORY);

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS) {
		ptr->l = mhead->mh_root;
	} else {
		xfs_fileoff_t		root = be64_to_cpu(mhead->mh_root);

		ptr->s = cpu_to_be32(root);
	}
}

struct xfs_btree_cur *
xfs_btree_mem_dup_cursor(
	struct xfs_btree_cur		*cur)
{
	struct xfs_btree_cur		*ncur;

	ASSERT(cur->bc_flags & XFS_BTREE_IN_MEMORY);

	ncur = xfs_btree_alloc_cursor(cur->bc_mp, cur->bc_tp, cur->bc_btnum);
	ncur->bc_flags = cur->bc_flags;
	ncur->bc_nlevels = cur->bc_nlevels;
	ncur->bc_statoff = cur->bc_statoff;
	ncur->bc_ops = cur->bc_ops;
	memcpy(&ncur->bc_mem, &cur->bc_mem, sizeof(cur->bc_mem));

	return ncur;
}

static xfs_failaddr_t
xfs_btree_mem_head_verify(
	struct xfs_buf			*bp)
{
	struct xfs_btree_mem_head	*mhead = bp->b_addr;

	if (!xfs_verify_magic(bp, mhead->mh_magic))
		return __this_address;
	if (be64_to_cpu(mhead->mh_root) == 0)
		return __this_address;
	if (be32_to_cpu(mhead->mh_nlevels) == 0)
		return __this_address;

	return NULL;
}

static void
xfs_btree_mem_head_read_verify(
	struct xfs_buf		*bp)
{
	xfs_failaddr_t		fa = xfs_btree_mem_head_verify(bp);

	if (fa)
		xfs_verifier_error(bp, -EFSCORRUPTED, fa);
}

static void
xfs_btree_mem_head_write_verify(
	struct xfs_buf		*bp)
{
	xfs_failaddr_t		fa = xfs_btree_mem_head_verify(bp);

	if (fa)
		xfs_verifier_error(bp, -EFSCORRUPTED, fa);
}

static const struct xfs_buf_ops xfs_btree_mem_head_buf_ops = {
	.name			= "xfs_btree_mem_head",
	.magic			= { cpu_to_be32(XFS_BTREE_MEM_HEAD_MAGIC),
				    cpu_to_be32(XFS_BTREE_MEM_HEAD_MAGIC) },
	.verify_read		= xfs_btree_mem_head_read_verify,
	.verify_write		= xfs_btree_mem_head_write_verify,
	.verify_struct		= xfs_btree_mem_head_verify,
};

/* Initialize the header block for an in-memory btree. */
static inline void
xfs_btree_mem_head_init(
	struct xfs_buf			*head_bp,
	xfs_fileoff_t			leaf_fileoff)
{
	struct xfs_btree_mem_head	*mhead = head_bp->b_addr;

	mhead->mh_magic = cpu_to_be32(XFS_BTREE_MEM_HEAD_MAGIC);
	mhead->mh_nlevels = cpu_to_be32(1);
	mhead->mh_root = cpu_to_be64(leaf_fileoff);
	head_bp->b_ops = &xfs_btree_mem_head_buf_ops;
}

/* Read the in-memory btree head. */
int
xfs_btree_mem_head_read_buf(
	struct xfs_buftarg		*btp,
	struct xfs_trans		*tp,
	struct xfs_buf			**bpp)
{
	struct xfs_mount		*mp = btp->bt_mount;

	return xfs_trans_read_buf(mp, tp, btp, XFS_BTREE_MEM_HEAD_DADDR, 1, 0,
			bpp, &xfs_btree_mem_head_buf_ops);
}

/* Return tree height from the in-memory btree head */
unsigned int
xfs_btree_mem_head_nlevels(
	struct xfs_buf			*head_bp)
{
	struct xfs_btree_mem_head	*mhead = head_bp->b_addr;

	return be32_to_cpu(mhead->mh_nlevels);
}

/* Extract the buftarg target for this xfile btree. */
struct xfs_buftarg *
xfbtree_target(struct xfbtree *xfbtree)
{
	return xfbtree->target;
}

/* Returns true if the pointer is within the xfile. */
bool
xfbtree_check_ptr(
	struct xfs_btree_cur	*cur,
	xfs_fileoff_t		xfileoff)
{
	ASSERT(cur->bc_flags & XFS_BTREE_IN_MEMORY);

	return xfileoff != 0;
}
