/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021-2023 Oracle.  All Rights Reserved.
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
#include "xfs_error.h"
#include "xfs_btree_mem.h"
#include "xfs_ag.h"
#include "scrub/scrub.h"
#include "scrub/xfile.h"
#include "scrub/xfbtree.h"
#include "scrub/bitmap.h"
#include "scrub/trace.h"

/* Bitmaps, but for type-checked for xfileoff_t */

static inline void xfoff_bitmap_init(struct xfoff_bitmap *bitmap)
{
	xbitmap_init(&bitmap->xfoffbitmap);
}

static inline void xfoff_bitmap_destroy(struct xfoff_bitmap *bitmap)
{
	xbitmap_destroy(&bitmap->xfoffbitmap);
}

static inline int xfoff_bitmap_set(struct xfoff_bitmap *bitmap,
		xfs_fileoff_t start, xfs_filblks_t len)
{
	return xbitmap_set(&bitmap->xfoffbitmap, start, len);
}

static inline int xfoff_bitmap_take_first_set(struct xfoff_bitmap *bitmap,
		xfileoff_t *valp)
{
	uint64_t	val;
	int		error;

	error = xbitmap_take_first_set(&bitmap->xfoffbitmap, 0, -1ULL, &val);
	if (error)
		return error;
	*valp = val;
	return 0;
}

/* btree ops functions for in-memory btrees. */

static xfs_failaddr_t
xfs_btree_mem_head_verify(
	struct xfs_buf			*bp)
{
	struct xfs_btree_mem_head	*mhead = bp->b_addr;
	struct xfs_mount		*mp = bp->b_mount;

	if (!xfs_verify_magic(bp, mhead->mh_magic))
		return __this_address;
	if (be32_to_cpu(mhead->mh_nlevels) == 0)
		return __this_address;
	if (!uuid_equal(&mhead->mh_uuid, &mp->m_sb.sb_meta_uuid))
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
	unsigned long long		owner,
	xfileoff_t			leaf_xfoff)
{
	struct xfs_btree_mem_head	*mhead = head_bp->b_addr;
	struct xfs_mount		*mp = head_bp->b_mount;

	mhead->mh_magic = cpu_to_be32(XFS_BTREE_MEM_HEAD_MAGIC);
	mhead->mh_nlevels = cpu_to_be32(1);
	mhead->mh_owner = cpu_to_be64(owner);
	mhead->mh_root = cpu_to_be64(leaf_xfoff);
	uuid_copy(&mhead->mh_uuid, &mp->m_sb.sb_meta_uuid);

	head_bp->b_ops = &xfs_btree_mem_head_buf_ops;
}

/* Return tree height from the in-memory btree head. */
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
	xfs_failaddr_t			fa = NULL;

	ASSERT(cur->bc_flags & XFS_BTREE_IN_XFILE);

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS)
		bt_xfoff = be64_to_cpu(ptr->l);
	else
		bt_xfoff = be32_to_cpu(ptr->s);

	if (!xfbtree_verify_xfileoff(cur, bt_xfoff)) {
		fa = __this_address;
		goto done;
	}

	/* Can't point to the head or anything before it */
	if (bt_xfoff < XFBTREE_INIT_LEAF_BLOCK) {
		fa = __this_address;
		goto done;
	}

done:
	if (fa) {
		xfs_err(cur->bc_mp,
"In-memory: Corrupt btree %d flags 0x%x pointer at level %d index %d fa %pS.",
				cur->bc_btnum, cur->bc_flags, level, index,
				fa);
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

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS)
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

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS)
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
	struct xfs_buf			*head_bp = cur->bc_mem.head_bp;
	struct xfs_btree_mem_head	*mhead = head_bp->b_addr;

	ASSERT(cur->bc_flags & XFS_BTREE_IN_XFILE);

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS) {
		mhead->mh_root = ptr->l;
	} else {
		uint32_t		root = be32_to_cpu(ptr->s);

		mhead->mh_root = cpu_to_be64(root);
	}
	be32_add_cpu(&mhead->mh_nlevels, inc);
	xfs_trans_log_buf(cur->bc_tp, head_bp, 0, sizeof(*mhead) - 1);
}

/* Initialize a pointer from the in-memory btree header. */
void
xfbtree_init_ptr_from_cur(
	struct xfs_btree_cur		*cur,
	union xfs_btree_ptr		*ptr)
{
	struct xfs_buf			*head_bp = cur->bc_mem.head_bp;
	struct xfs_btree_mem_head	*mhead = head_bp->b_addr;

	ASSERT(cur->bc_flags & XFS_BTREE_IN_XFILE);

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS) {
		ptr->l = mhead->mh_root;
	} else {
		uint64_t		root = be64_to_cpu(mhead->mh_root);

		ptr->s = cpu_to_be32(root);
	}
}

/* Duplicate an in-memory btree cursor. */
struct xfs_btree_cur *
xfbtree_dup_cursor(
	struct xfs_btree_cur		*cur)
{
	struct xfs_btree_cur		*ncur;

	ASSERT(cur->bc_flags & XFS_BTREE_IN_XFILE);

	ncur = xfs_btree_alloc_cursor(cur->bc_mp, cur->bc_tp, cur->bc_btnum,
			cur->bc_ops, cur->bc_maxlevels, cur->bc_cache);
	ncur->bc_flags = cur->bc_flags;
	ncur->bc_nlevels = cur->bc_nlevels;
	ncur->bc_statoff = cur->bc_statoff;

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

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS) {
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
	ASSERT(cur->bc_flags & XFS_BTREE_IN_XFILE);

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

/* Close the btree xfile and release all resources. */
void
xfbtree_destroy(
	struct xfbtree		*xfbt)
{
	xfoff_bitmap_destroy(&xfbt->freespace);
	xfs_buftarg_drain(xfbt->target);
	kfree(xfbt);
}

/* Compute the number of bytes available for records. */
static inline unsigned int
xfbtree_rec_bytes(
	struct xfs_mount		*mp,
	const struct xfbtree_config	*cfg)
{
	unsigned int			blocklen = xfo_to_b(1);

	if (cfg->btree_ops->geom_flags & XFS_BTREE_LONG_PTRS) {
		if (xfs_has_crc(mp))
			return blocklen - XFS_BTREE_LBLOCK_CRC_LEN;

		return blocklen - XFS_BTREE_LBLOCK_LEN;
	}

	if (xfs_has_crc(mp))
		return blocklen - XFS_BTREE_SBLOCK_CRC_LEN;

	return blocklen - XFS_BTREE_SBLOCK_LEN;
}

/* Initialize an empty leaf block as the btree root. */
STATIC int
xfbtree_init_leaf_block(
	struct xfs_mount		*mp,
	struct xfbtree			*xfbt,
	const struct xfbtree_config	*cfg)
{
	struct xfs_buf			*bp;
	xfs_daddr_t			daddr;
	int				error;

	daddr = xfo_to_daddr(XFBTREE_INIT_LEAF_BLOCK);
	error = xfs_buf_get(xfbt->target, daddr, xfbtree_bbsize(), &bp);
	if (error)
		return error;

	trace_xfbtree_create_root_buf(xfbt, bp);

	xfs_btree_init_buf(mp, bp, cfg->btree_ops, 0, 0, cfg->owner);
	error = xfs_bwrite(bp);
	xfs_buf_relse(bp);
	if (error)
		return error;

	xfbt->highest_offset++;
	return 0;
}

/* Initialize the in-memory btree header block. */
STATIC int
xfbtree_init_head(
	struct xfbtree		*xfbt)
{
	struct xfs_buf		*bp;
	xfs_daddr_t		daddr;
	int			error;

	daddr = xfo_to_daddr(XFBTREE_HEAD_BLOCK);
	error = xfs_buf_get(xfbt->target, daddr, xfbtree_bbsize(), &bp);
	if (error)
		return error;

	xfs_btree_mem_head_init(bp, xfbt->owner, XFBTREE_INIT_LEAF_BLOCK);
	error = xfs_bwrite(bp);
	xfs_buf_relse(bp);
	if (error)
		return error;

	xfbt->highest_offset++;
	return 0;
}

/* Create an xfile btree backing thing that can be used for in-memory btrees. */
int
xfbtree_create(
	struct xfs_mount		*mp,
	const struct xfbtree_config	*cfg,
	struct xfbtree			**xfbtreep)
{
	struct xfbtree			*xfbt;
	unsigned int			blocklen = xfbtree_rec_bytes(mp, cfg);
	unsigned int			keyptr_len = cfg->btree_ops->key_len;
	int				error;

	/* Requires an xfile-backed buftarg. */
	if (!(cfg->target->bt_flags & XFS_BUFTARG_XFILE)) {
		ASSERT(cfg->target->bt_flags & XFS_BUFTARG_XFILE);
		return -EINVAL;
	}

	xfbt = kzalloc(sizeof(struct xfbtree), XCHK_GFP_FLAGS);
	if (!xfbt)
		return -ENOMEM;
	xfbt->target = cfg->target;
	xfoff_bitmap_init(&xfbt->freespace);

	/* Set up min/maxrecs for this btree. */
	if (cfg->btree_ops->geom_flags & XFS_BTREE_LONG_PTRS)
		keyptr_len += sizeof(__be64);
	else
		keyptr_len += sizeof(__be32);
	xfbt->maxrecs[0] = blocklen / cfg->btree_ops->rec_len;
	xfbt->maxrecs[1] = blocklen / keyptr_len;
	xfbt->minrecs[0] = xfbt->maxrecs[0] / 2;
	xfbt->minrecs[1] = xfbt->maxrecs[1] / 2;
	xfbt->owner = cfg->owner;

	/* Initialize the empty btree. */
	error = xfbtree_init_leaf_block(mp, xfbt, cfg);
	if (error)
		goto err_freesp;

	error = xfbtree_init_head(xfbt);
	if (error)
		goto err_freesp;

	trace_xfbtree_create(mp, cfg, xfbt);

	*xfbtreep = xfbt;
	return 0;

err_freesp:
	xfoff_bitmap_destroy(&xfbt->freespace);
	xfs_buftarg_drain(xfbt->target);
	kfree(xfbt);
	return error;
}

/* Read the in-memory btree head. */
int
xfbtree_head_read_buf(
	struct xfbtree		*xfbt,
	struct xfs_trans	*tp,
	struct xfs_buf		**bpp)
{
	struct xfs_buftarg	*btp = xfbt->target;
	struct xfs_mount	*mp = btp->bt_mount;
	struct xfs_btree_mem_head *mhead;
	struct xfs_buf		*bp;
	xfs_daddr_t		daddr;
	int			error;

	daddr = xfo_to_daddr(XFBTREE_HEAD_BLOCK);
	error = xfs_trans_read_buf(mp, tp, btp, daddr, xfbtree_bbsize(), 0,
			&bp, &xfs_btree_mem_head_buf_ops);
	if (error)
		return error;

	mhead = bp->b_addr;
	if (be64_to_cpu(mhead->mh_owner) != xfbt->owner) {
		xfs_verifier_error(bp, -EFSCORRUPTED, __this_address);
		xfs_trans_brelse(tp, bp);
		return -EFSCORRUPTED;
	}

	*bpp = bp;
	return 0;
}

static inline struct xfile *xfbtree_xfile(struct xfbtree *xfbt)
{
	return xfbt->target->bt_xfile;
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
	xfileoff_t			bt_xfoff;
	loff_t				pos;
	int				error;

	ASSERT(cur->bc_flags & XFS_BTREE_IN_XFILE);

	/*
	 * Find the first free block in the free space bitmap and take it.  If
	 * none are found, seek to end of the file.
	 */
	error = xfoff_bitmap_take_first_set(&xfbt->freespace, &bt_xfoff);
	if (error == -ENODATA) {
		bt_xfoff = xfbt->highest_offset++;
		error = 0;
	}
	if (error)
		return error;

	trace_xfbtree_alloc_block(xfbt, cur, bt_xfoff);

	/* Fail if the block address exceeds the maximum for short pointers. */
	if (!(cur->bc_flags & XFS_BTREE_LONG_PTRS) && bt_xfoff >= INT_MAX) {
		*stat = 0;
		return 0;
	}

	/* Make sure we actually can write to the block before we return it. */
	pos = xfo_to_b(bt_xfoff);
	error = xfile_prealloc(xfbtree_xfile(xfbt), pos, xfo_to_b(1));
	if (error)
		return error;

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS)
		new->l = cpu_to_be64(bt_xfoff);
	else
		new->s = cpu_to_be32(bt_xfoff);

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
	xfileoff_t		bt_xfoff, bt_xflen;

	ASSERT(cur->bc_flags & XFS_BTREE_IN_XFILE);

	bt_xfoff = xfs_daddr_to_xfot(xfs_buf_daddr(bp));
	bt_xflen = xfs_daddr_to_xfot(bp->b_length);

	trace_xfbtree_free_block(xfbt, cur, bt_xfoff);

	return xfoff_bitmap_set(&xfbt->freespace, bt_xfoff, bt_xflen);
}

/* Return the minimum number of records for a btree block. */
int
xfbtree_get_minrecs(
	struct xfs_btree_cur	*cur,
	int			level)
{
	struct xfbtree		*xfbt = cur->bc_mem.xfbtree;

	return xfbt->minrecs[level != 0];
}

/* Return the maximum number of records for a btree block. */
int
xfbtree_get_maxrecs(
	struct xfs_btree_cur	*cur,
	int			level)
{
	struct xfbtree		*xfbt = cur->bc_mem.xfbtree;

	return xfbt->maxrecs[level != 0];
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
 * Detach this (probably dirty) xfbtree buffer from the transaction by any
 * means necessary.  Returns true if the buffer needs to be written.
 */
STATIC bool
xfbtree_trans_bdetach(
	struct xfs_trans	*tp,
	struct xfs_buf		*bp)
{
	struct xfs_buf_log_item	*bli = bp->b_log_item;
	bool			dirty;

	ASSERT(bli != NULL);

	dirty = bli->bli_flags & (XFS_BLI_DIRTY | XFS_BLI_ORDERED);

	bli->bli_flags &= ~(XFS_BLI_DIRTY | XFS_BLI_ORDERED |
			    XFS_BLI_LOGGED | XFS_BLI_STALE);
	clear_bit(XFS_LI_DIRTY, &bli->bli_item.li_flags);

	while (bp->b_log_item != NULL)
		xfs_trans_bdetach(tp, bp);

	return dirty;
}

/*
 * Commit changes to the incore btree immediately by writing all dirty xfbtree
 * buffers to the backing xfile.  This detaches all xfbtree buffers from the
 * transaction, even on failure.  The buffer locks are dropped between the
 * delwri queue and submit, so the caller must synchronize btree access.
 *
 * Normally we'd let the buffers commit with the transaction and get written to
 * the xfile via the log, but online repair stages ephemeral btrees in memory
 * and uses the btree_staging functions to write new btrees to disk atomically.
 * The in-memory btree (and its backing store) are discarded at the end of the
 * repair phase, which means that xfbtree buffers cannot commit with the rest
 * of a transaction.
 *
 * In other words, online repair only needs the transaction to collect buffer
 * pointers and to avoid buffer deadlocks, not to guarantee consistency of
 * updates.
 */
int
xfbtree_trans_commit(
	struct xfbtree		*xfbt,
	struct xfs_trans	*tp)
{
	LIST_HEAD(buffer_list);
	struct xfs_log_item	*lip, *n;
	bool			corrupt = false;
	bool			tp_dirty = false;

	/*
	 * For each xfbtree buffer attached to the transaction, write the dirty
	 * buffers to the xfile and release them.
	 */
	list_for_each_entry_safe(lip, n, &tp->t_items, li_trans) {
		struct xfs_buf	*bp = xfbtree_buf_match(xfbt, lip);
		bool		dirty;

		if (!bp) {
			if (test_bit(XFS_LI_DIRTY, &lip->li_flags))
				tp_dirty |= true;
			continue;
		}

		trace_xfbtree_trans_commit_buf(xfbt, bp);

		dirty = xfbtree_trans_bdetach(tp, bp);
		if (dirty && !corrupt) {
			xfs_failaddr_t	fa = bp->b_ops->verify_struct(bp);

			/*
			 * Because this btree is ephemeral, validate the buffer
			 * structure before delwri_submit so that we can return
			 * corruption errors to the caller without shutting
			 * down the filesystem.
			 *
			 * If the buffer fails verification, log the failure
			 * but continue walking the transaction items so that
			 * we remove all ephemeral btree buffers.
			 */
			if (fa) {
				corrupt = true;
				xfs_verifier_error(bp, -EFSCORRUPTED, fa);
			} else {
				xfs_buf_delwri_queue_here(bp, &buffer_list);
			}
		}

		xfs_buf_relse(bp);
	}

	/*
	 * Reset the transaction's dirty flag to reflect the dirty state of the
	 * log items that are still attached.
	 */
	tp->t_flags = (tp->t_flags & ~XFS_TRANS_DIRTY) |
			(tp_dirty ? XFS_TRANS_DIRTY : 0);

	if (corrupt) {
		xfs_buf_delwri_cancel(&buffer_list);
		return -EFSCORRUPTED;
	}

	if (list_empty(&buffer_list))
		return 0;

	return xfs_buf_delwri_submit(&buffer_list);
}

/*
 * Cancel changes to the incore btree by detaching all the xfbtree buffers.
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

		xfbtree_trans_bdetach(tp, bp);
		xfs_buf_relse(bp);
	}

	/*
	 * Reset the transaction's dirty flag to reflect the dirty state of the
	 * log items that are still attached.
	 */
	tp->t_flags = (tp->t_flags & ~XFS_TRANS_DIRTY) |
			(tp_dirty ? XFS_TRANS_DIRTY : 0);
}
