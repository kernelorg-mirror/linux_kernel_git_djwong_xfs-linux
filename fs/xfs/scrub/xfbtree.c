/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
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
#include "xfs_error.h"
#include "xfs_btree_mem.h"
#include "xfs_ag.h"
#include "scrub/xfile.h"
#include "scrub/xfbtree.h"

/* Extract the buftarg target for this xfile btree. */
struct xfs_buftarg *
xfbtree_target(struct xfbtree *xfbtree)
{
	return xfbtree->target;
}

/* Is this daddr (sector offset) contained within the buffer target? */
static inline bool
xfbtree_verify_buftarg_xfileoff(
	struct xfs_buftarg	*btp,
	xfileoff_t		xfoff)
{
	xfs_daddr_t		xfoff_daddr = xfo_to_daddr(xfoff);

	return xfs_buftarg_verify_daddr(btp, xfoff_daddr);
}

/* Is this btree xfile offset contained within the xfile? */
bool
xfbtree_verify_xfileoff(
	struct xfs_btree_cur	*cur,
	unsigned long long	xfoff)
{
	struct xfs_buftarg	*btp = xfbtree_target(cur->bc_mem.xfbtree);

	return xfbtree_verify_buftarg_xfileoff(btp, xfoff);
}

/* Check if a btree pointer is reasonable. */
int
xfbtree_check_ptr(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr,
	int				index,
	int				level)
{
	xfileoff_t			bt_xfoff;

	ASSERT(cur->bc_ops->geom_flags & XFS_BTGEO_IN_XFILE);

	if (cur->bc_ops->geom_flags & XFS_BTGEO_LONG_PTRS)
		bt_xfoff = be64_to_cpu(ptr->l);
	else
		bt_xfoff = be32_to_cpu(ptr->s);

	if (!xfbtree_verify_xfileoff(cur, bt_xfoff)) {
		xfs_err(cur->bc_mp,
"In-memory: Corrupt btree %d flags 0x%x pointer at level %d index %d fa %pS.",
				cur->bc_btnum, cur->bc_flags, level, index,
				__this_address);
		return -EFSCORRUPTED;
	}
	return 0;
}

/* Convert a btree pointer to a daddr */
xfs_daddr_t
xfbtree_ptr_to_daddr(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr)
{
	xfileoff_t			bt_xfoff;

	if (cur->bc_ops->geom_flags & XFS_BTGEO_LONG_PTRS)
		bt_xfoff = be64_to_cpu(ptr->l);
	else
		bt_xfoff = be32_to_cpu(ptr->s);
	return xfo_to_daddr(bt_xfoff);
}

/* Set the pointer to point to this buffer. */
void
xfbtree_buf_to_ptr(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp,
	union xfs_btree_ptr	*ptr)
{
	xfileoff_t		xfoff = xfs_daddr_to_xfo(xfs_buf_daddr(bp));

	if (cur->bc_ops->geom_flags & XFS_BTGEO_LONG_PTRS)
		ptr->l = cpu_to_be64(xfoff);
	else
		ptr->s = cpu_to_be32(xfoff);
}

/* Return the in-memory btree block size, in units of 512 bytes. */
unsigned int xfbtree_bbsize(void)
{
	return xfo_to_daddr(1);
}

/* Set the root of an in-memory btree. */
void
xfbtree_set_root(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr,
	int				inc)
{
	ASSERT(cur->bc_ops->geom_flags & XFS_BTGEO_IN_XFILE);

	cur->bc_mem.xfbtree->root = *ptr;
	cur->bc_mem.xfbtree->nlevels += inc;
}

/* Initialize a pointer from the in-memory btree header. */
void
xfbtree_init_ptr_from_cur(
	struct xfs_btree_cur		*cur,
	union xfs_btree_ptr		*ptr)
{
	ASSERT(cur->bc_ops->geom_flags & XFS_BTGEO_IN_XFILE);

	*ptr = cur->bc_mem.xfbtree->root;
}

/* Duplicate an in-memory btree cursor. */
struct xfs_btree_cur *
xfbtree_dup_cursor(
	struct xfs_btree_cur		*cur)
{
	struct xfs_btree_cur		*ncur;

	ASSERT(cur->bc_ops->geom_flags & XFS_BTGEO_IN_XFILE);

	ncur = xfs_btree_alloc_cursor(cur->bc_mp, cur->bc_tp, cur->bc_btnum,
			cur->bc_ops, cur->bc_maxlevels, cur->bc_cache);
	ncur->bc_flags = cur->bc_flags;
	ncur->bc_nlevels = cur->bc_nlevels;
	memcpy(&ncur->bc_mem, &cur->bc_mem, sizeof(cur->bc_mem));

	if (cur->bc_mem.pag)
		ncur->bc_mem.pag = xfs_perag_hold(cur->bc_mem.pag);

	return ncur;
}

/* Check the owner of an in-memory btree block. */
xfs_failaddr_t
xfbtree_check_block_owner(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block)
{
	struct xfbtree		*xfbt = cur->bc_mem.xfbtree;

	if (cur->bc_ops->geom_flags & XFS_BTGEO_LONG_PTRS) {
		if (be64_to_cpu(block->bb_u.l.bb_owner) != xfbt->owner)
			return __this_address;

		return NULL;
	}

	if (be32_to_cpu(block->bb_u.s.bb_owner) != xfbt->owner)
		return __this_address;

	return NULL;
}

/* Return the owner of this in-memory btree. */
unsigned long long
xfbtree_owner(
	struct xfs_btree_cur	*cur)
{
	return cur->bc_mem.xfbtree->owner;
}

/* Return the xfile offset (in blocks) of a btree buffer. */
unsigned long long
xfbtree_buf_to_xfoff(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp)
{
	ASSERT(cur->bc_ops->geom_flags & XFS_BTGEO_IN_XFILE);

	return xfs_daddr_to_xfo(xfs_buf_daddr(bp));
}

/* Verify a long-format btree block. */
xfs_failaddr_t
xfbtree_lblock_verify(
	struct xfs_buf		*bp,
	unsigned int		max_recs)
{
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	struct xfs_buftarg	*btp = bp->b_target;

	/* numrecs verification */
	if (be16_to_cpu(block->bb_numrecs) > max_recs)
		return __this_address;

	/* sibling pointer verification */
	if (block->bb_u.l.bb_leftsib != cpu_to_be64(NULLFSBLOCK) &&
	    !xfbtree_verify_buftarg_xfileoff(btp,
				be64_to_cpu(block->bb_u.l.bb_leftsib)))
		return __this_address;

	if (block->bb_u.l.bb_rightsib != cpu_to_be64(NULLFSBLOCK) &&
	    !xfbtree_verify_buftarg_xfileoff(btp,
				be64_to_cpu(block->bb_u.l.bb_rightsib)))
		return __this_address;

	return NULL;
}

/* Verify a short-format btree block. */
xfs_failaddr_t
xfbtree_sblock_verify(
	struct xfs_buf		*bp,
	unsigned int		max_recs)
{
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	struct xfs_buftarg	*btp = bp->b_target;

	/* numrecs verification */
	if (be16_to_cpu(block->bb_numrecs) > max_recs)
		return __this_address;

	/* sibling pointer verification */
	if (block->bb_u.s.bb_leftsib != cpu_to_be32(NULLAGBLOCK) &&
	    !xfbtree_verify_buftarg_xfileoff(btp,
				be32_to_cpu(block->bb_u.s.bb_leftsib)))
		return __this_address;

	if (block->bb_u.s.bb_rightsib != cpu_to_be32(NULLAGBLOCK) &&
	    !xfbtree_verify_buftarg_xfileoff(btp,
				be32_to_cpu(block->bb_u.s.bb_rightsib)))
		return __this_address;

	return NULL;
}
