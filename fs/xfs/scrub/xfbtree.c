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
#include "xfs_bit.h"
#include "xfs_mount.h"
#include "xfs_trans.h"
#include "xfs_buf_item.h"
#include "xfs_btree.h"
#include "xfs_btree_mem.h"
#include "xfs_error.h"
#include "scrub/scrub.h"
#include "scrub/xfile.h"
#include "scrub/xfbtree.h"
#include "scrub/bitmap.h"
#include "scrub/trace.h"

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

	return xfs_trans_read_buf(mp, tp, btp, XFS_BTREE_MEM_HEAD_DADDR,
			XFS_FSB_TO_BB(mp, 1), 0, bpp,
			&xfs_btree_mem_head_buf_ops);
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
	struct xfile_stat	sb;
	struct xfbtree		*xfbtree = cur->bc_mem.xfbtree;
	int			error;

	ASSERT(cur->bc_flags & XFS_BTREE_IN_MEMORY);

	if (xfileoff == 0)
		return false;

	error = xfile_stat(xfbtree->xfile, &sb);
	if (error)
		return false;

	return xfileoff < XFS_B_TO_FSB(cur->bc_mp, sb.size);
}

/* Close the btree xfile and release all resources. */
void
xfbtree_destroy(
	struct xfbtree		*xfbt)
{
	xbitmap_destroy(xfbt->freespace);
	kmem_free(xfbt->freespace);
	xfs_buftarg_drain(xfbt->target);
	xfs_free_buftarg(xfbt->target);
	xfile_destroy(xfbt->xfile);
	kmem_free(xfbt);
}

/* Create an xfile btree backing thing that can be used for in-memory btrees. */
struct xfbtree *
xfbtree_create(
	struct xfs_mount	*mp,
	xfs_btnum_t		btnum,
	const struct xfs_buf_ops *b_ops,
	unsigned int		flags,
	const char		*description)
{
	struct xfbtree		*xfbt;
	struct xfs_buf		*bp;
	xfs_daddr_t		daddr;
	unsigned int		bc_flags = 0;
	int			error;

	if (flags & XFBTREE_CREATE_LONG_PTRS)
		bc_flags |= XFS_BTREE_LONG_PTRS;

	xfbt = kmem_zalloc(sizeof(struct xfbtree), KM_NOFS | KM_MAYFAIL);
	if (!xfbt)
		return ERR_PTR(-ENOMEM);

	xfbt->xfile = xfile_create(description, 0);
	if (IS_ERR(xfbt->xfile))
		return ERR_PTR(PTR_ERR(xfbt->xfile));

	xfbt->target = xfs_alloc_memory_buftarg(mp, xfbt->xfile);
	if (!xfbt->target) {
		error = -ENOMEM;
		goto err_xfile;
	}

	if (flags & XFBTREE_DIRECT_MAP)
		xfbt->target->bt_flags |= XFS_BUFTARG_DIRECT_MAP;

	xfbt->freespace = kmem_alloc(sizeof(struct xbitmap),
			KM_NOFS | KM_MAYFAIL);
	if (!xfbt->freespace) {
		error = -ENOMEM;
		goto err_buftarg;
	}
	xbitmap_init(xfbt->freespace);

	/* Initialize an empty leaf block as the btree root. */
	daddr = XFS_FSB_TO_DADDR(mp, XFBTREE_NEW_LEAF_FILEOFF);
	error = xfs_buf_get(xfbt->target, daddr, mp->m_bsize, &bp);
	if (error)
		goto err_freesp;

	trace_xfbtree_create_root_buf(xfbt, bp);

	bp->b_ops = b_ops;
	xfs_btree_init_block_int(mp, bp->b_addr, daddr, btnum, 0, 0, 0,
			bc_flags);
	error = xfs_bwrite(bp);
	xfs_buf_relse(bp);
	if (error)
		goto err_freesp;

	/* Initialize the in-memory btree header block. */
	error = xfs_buf_get(xfbt->target, XFS_BTREE_MEM_HEAD_DADDR,
			XFS_FSB_TO_BB(mp, 1), &bp);
	if (error)
		goto err_freesp;

	xfs_btree_mem_head_init(bp, XFBTREE_NEW_LEAF_FILEOFF);
	error = xfs_bwrite(bp);
	xfs_buf_relse(bp);
	if (error)
		goto err_freesp;

	return xfbt;

err_freesp:
	xbitmap_destroy(xfbt->freespace);
	kmem_free(xfbt->freespace);
err_buftarg:
	xfs_buftarg_drain(xfbt->target);
	xfs_free_buftarg(xfbt->target);
err_xfile:
	xfile_destroy(xfbt->xfile);
	return ERR_PTR(error);
}

/* Allocate a block to our in-memory btree. */
int
xfbtree_alloc_block(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*start,
	union xfs_btree_ptr		*new,
	int				*stat)
{
	struct xfbtree			*xfbt = cur->bc_mem.xfbtree;
	struct xfs_mount		*mp = cur->bc_mp;
	xfs_fileoff_t			fileoff;
	loff_t				pos;
	int				error;

	ASSERT(cur->bc_flags & XFS_BTREE_IN_MEMORY);

	/*
	 * Find the first free block in the free space bitmap and take it.  If
	 * none are found, seek to end of the file.
	 */
	fileoff = xbitmap_take_first_set(xfbt->freespace, 0, -1ULL);
	if (fileoff == -1ULL) {
		struct xfile_stat	statbuf;

		error = xfile_stat(xfbt->xfile, &statbuf);
		if (error)
			return error;

		fileoff = XFS_B_TO_FSB(mp, statbuf.size);
	}

	trace_xfbtree_alloc_block(xfbt, cur, fileoff);

	/* Block address exceeds the maximum for short pointers. */
	if (!(cur->bc_flags & XFS_BTREE_LONG_PTRS) && fileoff >= INT_MAX) {
		*stat = 0;
		return 0;
	}

	/* Make sure we actually can write to the block before we return it. */
	pos = XFS_FSB_TO_B(mp, fileoff);
	error = xfile_prealloc(xfbt->xfile, pos, mp->m_sb.sb_blocksize);
	if (error)
		return error;

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS)
		new->l = cpu_to_be64(fileoff);
	else
		new->s = cpu_to_be32(fileoff);

	*stat = 1;
	return 0;
}

/* Free a block from our in-memory btree. */
int
xfbtree_free_block(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp)
{
	struct xfbtree		*xfbt = cur->bc_mem.xfbtree;
	xfs_fileoff_t		fileoff;
	loff_t			pos, len;
	int			error;

	ASSERT(cur->bc_flags & XFS_BTREE_IN_MEMORY);

	pos = BBTOB(xfs_buf_daddr(bp));
	len = BBTOB(bp->b_length);
	fileoff = XFS_B_TO_FSBT(cur->bc_mp, pos);

	trace_xfbtree_free_block(xfbt, cur, fileoff);

	error = xbitmap_set(xfbt->freespace, fileoff,
			XFS_B_TO_FSBT(cur->bc_mp, len));
	if (error)
		return error;

	xfile_discard(xfbt->xfile, pos, len);
	return 0;
}

/* If this log item is a buffer item that came from the xfbtree, return it. */
static inline struct xfs_buf *
xfbtree_buf_match(
	struct xfbtree			*xfbt,
	const struct xfs_log_item	*lip)
{
	const struct xfs_buf_log_item	*bli;
	struct xfs_buf			*bp;

	if (lip->li_type != XFS_LI_BUF)
		return NULL;

	bli = container_of(lip, struct xfs_buf_log_item, bli_item);
	bp = bli->bli_buf;
	if (bp->b_target != xfbt->target)
		return NULL;

	return bp;
}

/*
 * Clear all the dirty/stale state that will prevent us from detaching the
 * buffer from the transaction, then remove the buffer from the transaction.
 */
STATIC void
xfbtree_trans_brelse(
	struct xfs_trans	*tp,
	struct xfs_buf		*bp)
{
	struct xfs_buf_log_item	*bli = bp->b_log_item;

	ASSERT(bli != NULL);

	bli->bli_flags &= ~(XFS_BLI_DIRTY | XFS_BLI_ORDERED |
			    XFS_BLI_LOGGED | XFS_BLI_STALE);
	clear_bit(XFS_LI_DIRTY, &bli->bli_item.li_flags);

	while (bp->b_log_item != NULL)
		xfs_trans_brelse(tp, bp);
}

/*
 * Cancel changes to the incore btree by releasing all the xfbtree buffers.
 * Changes are not written to the backing store.  This is needed for online
 * repair btrees, which are by nature ephemeral.
 */
void
xfbtree_trans_cancel(
	struct xfbtree		*xfbt,
	struct xfs_trans	*tp)
{
	struct xfs_log_item	*lip, *n;
	bool			tp_dirty = false;

	list_for_each_entry_safe(lip, n, &tp->t_items, li_trans) {
		struct xfs_buf	*bp = xfbtree_buf_match(xfbt, lip);

		if (!bp) {
			if (test_bit(XFS_LI_DIRTY, &lip->li_flags))
				tp_dirty |= true;
			continue;
		}

		trace_xfbtree_trans_cancel_buf(xfbt, bp);
		xfbtree_trans_brelse(tp, bp);
	}

	/*
	 * Reset the transaction's dirty flag to reflect the dirty state of the
	 * log items that are still attached.
	 */
	tp->t_flags = (tp->t_flags & ~XFS_TRANS_DIRTY) |
			(tp_dirty ? XFS_TRANS_DIRTY : 0);
}

/* Write this xfbtree buffer to the backing xfile if it's dirty. */
static inline void
xfbtree_buf_delwri_queue(
	struct xfs_buf		*bp,
	struct list_head	*buffer_list)
{
	struct xfs_buf_log_item	*bli = bp->b_log_item;

	ASSERT(bli != NULL);

	if (!(bli->bli_flags & (XFS_BLI_DIRTY | XFS_BLI_ORDERED)))
		return;

	xfs_buf_delwri_queue(bp, buffer_list);
}

/*
 * Commit changes to the incore btree immediately by writing all dirty xfbtree
 * buffers to the backing xfile.  This releases all xfbtree buffers from the
 * transaction, even on failure.  The buffer locks are dropped between the
 * release and the writeback, so the caller must synchronize btree access.
 *
 * Normally we'd let the buffers commit with the transaction and get written to
 * the xfile via the log, but online repair stages ephemeral btrees in memory
 * and uses the btree_staging functions to write new btrees to disk atomically.
 * In other words, online repair only needs the transaction to collect buffer
 * pointers, not to guarantee updates.
 */
int
xfbtree_trans_commit(
	struct xfbtree		*xfbt,
	struct xfs_trans	*tp)
{
	LIST_HEAD(buffer_list);
	struct xfs_log_item	*lip, *n;
	bool			tp_dirty = false;

	/*
	 * For each xfbtree buffer attached to the transaction, write the dirty
	 * buffers to the xfile and release them.
	 */
	list_for_each_entry_safe(lip, n, &tp->t_items, li_trans) {
		struct xfs_buf	*bp = xfbtree_buf_match(xfbt, lip);

		if (!bp) {
			if (test_bit(XFS_LI_DIRTY, &lip->li_flags))
				tp_dirty |= true;
			continue;
		}

		trace_xfbtree_trans_commit_buf(xfbt, bp);
		xfbtree_buf_delwri_queue(bp, &buffer_list);
		xfbtree_trans_brelse(tp, bp);
	}

	/*
	 * Reset the transaction's dirty flag to reflect the dirty state of the
	 * log items that are still attached.
	 */
	tp->t_flags = (tp->t_flags & ~XFS_TRANS_DIRTY) |
			(tp_dirty ? XFS_TRANS_DIRTY : 0);

	if (list_empty(&buffer_list))
		return 0;

	return xfs_buf_delwri_submit(&buffer_list);
}
