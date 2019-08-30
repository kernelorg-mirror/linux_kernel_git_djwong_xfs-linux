// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_btree.h"
#include "scrub/bitmap.h"

/* Iterate each interval of a bitmap.  Do not change the bitmap. */
#define for_each_xbitmap_extent(itn, bitmap) \
	for ((itn) = rb_entry_safe(rb_first(&(bitmap)->root.rb_root), \
				   struct interval_tree_node, rb); \
	     (itn) != NULL; \
	     (itn) = rb_entry_safe(rb_next(&(itn)->rb), \
				   struct interval_tree_node, rb))

/* Clear a range of this bitmap. */
void
xbitmap_clear(
	struct xbitmap			*bitmap,
	uint64_t			start,
	uint64_t			len)
{
	struct interval_tree_node	*itn;
	uint64_t			last = start + len - 1;

	while ((itn = interval_tree_iter_first(&bitmap->root, start, last))) {
		if (itn->start < start) {
			/* overlaps with the left side of the clearing range */
			interval_tree_remove(itn, &bitmap->root);
			itn->last = start - 1;
			interval_tree_insert(itn, &bitmap->root);
		} else if (itn->last > last) {
			/* overlaps with the right side of the clearing range */
			interval_tree_remove(itn, &bitmap->root);
			itn->start = last + 1;
			interval_tree_insert(itn, &bitmap->root);
			break;
		} else {
			/* in the middle of the clearing range */
			interval_tree_remove(itn, &bitmap->root);
			kmem_free(itn);
		}
	}
}

/* Set a range of this bitmap. */
int
xbitmap_set(
	struct xbitmap			*bitmap,
	uint64_t			start,
	uint64_t			len)
{
	struct interval_tree_node	*left;
	struct interval_tree_node	*right;
	uint64_t			last = start + len - 1;

	/* Is this whole range already set? */
	left = interval_tree_iter_first(&bitmap->root, start, last);
	if (left && left->start <= start && left->last >= last)
		return 0;

	/* Clear out everything in the range we want to set. */
	xbitmap_clear(bitmap, start, len);

	/* Do we have a left-adjacent extent? */
	left = interval_tree_iter_first(&bitmap->root, start - 1, start - 1);
	ASSERT(!left || left->last + 1 == start);

	/* Do we have a right-adjacent extent? */
	right = interval_tree_iter_first(&bitmap->root, last + 1, last + 1);
	ASSERT(!right || right->start == last + 1);

	if (left && right) {
		/* combine left and right adjacent extent */
		interval_tree_remove(left, &bitmap->root);
		interval_tree_remove(right, &bitmap->root);
		left->last = right->last;
		interval_tree_insert(left, &bitmap->root);
		kmem_free(right);
	} else if (left) {
		/* combine with left extent */
		interval_tree_remove(left, &bitmap->root);
		left->last = last;
		interval_tree_insert(left, &bitmap->root);
	} else if (right) {
		/* combine with right extent */
		interval_tree_remove(right, &bitmap->root);
		right->start = start;
		interval_tree_insert(right, &bitmap->root);
	} else {
		/* add an extent */
		left = kmem_alloc(sizeof(struct interval_tree_node),
				KM_MAYFAIL);
		if (!left)
			return -ENOMEM;
		left->start = start;
		left->last = last;
		interval_tree_insert(left, &bitmap->root);
	}

	return 0;
}

/* Free everything related to this bitmap. */
void
xbitmap_destroy(
	struct xbitmap			*bitmap)
{
	struct interval_tree_node	*itn;

	while ((itn = interval_tree_iter_first(&bitmap->root, 0, -1ULL))) {
		interval_tree_remove(itn, &bitmap->root);
		kfree(itn);
	}
}

/* Set up a per-AG block bitmap. */
void
xbitmap_init(
	struct xbitmap		*bitmap)
{
	bitmap->root = RB_ROOT_CACHED;
}

/*
 * Remove all the blocks mentioned in @sub from the extents in @bitmap.
 *
 * The intent is that callers will iterate the rmapbt for all of its records
 * for a given owner to generate @bitmap; and iterate all the blocks of the
 * metadata structures that are not being rebuilt and have the same rmapbt
 * owner to generate @sub.  This routine subtracts all the extents
 * mentioned in sub from all the extents linked in @bitmap, which leaves
 * @bitmap as the list of blocks that are not accounted for, which we assume
 * are the dead blocks of the old metadata structure.  The blocks mentioned in
 * @bitmap can be reaped.
 *
 * This is the logical equivalent of bitmap &= ~sub.
 */
void
xbitmap_disunion(
	struct xbitmap			*bitmap,
	struct xbitmap			*sub)
{
	struct interval_tree_node	*itn;

	if (xbitmap_empty(bitmap) || xbitmap_empty(sub))
		return;

	for_each_xbitmap_extent(itn, sub)
		xbitmap_clear(bitmap, itn->start, itn->last - itn->start + 1);
}

/*
 * Record all btree blocks seen while iterating all records of a btree.
 *
 * We know that the btree query_all function starts at the left edge and walks
 * towards the right edge of the tree.  Therefore, we know that we can walk up
 * the btree cursor towards the root; if the pointer for a given level points
 * to the first record/key in that block, we haven't seen this block before;
 * and therefore we need to remember that we saw this block in the btree.
 *
 * So if our btree is:
 *
 *    4
 *  / | \
 * 1  2  3
 *
 * Pretend for this example that each leaf block has 100 btree records.  For
 * the first btree record, we'll observe that bc_ptrs[0] == 1, so we record
 * that we saw block 1.  Then we observe that bc_ptrs[1] == 1, so we record
 * block 4.  The list is [1, 4].
 *
 * For the second btree record, we see that bc_ptrs[0] == 2, so we exit the
 * loop.  The list remains [1, 4].
 *
 * For the 101st btree record, we've moved onto leaf block 2.  Now
 * bc_ptrs[0] == 1 again, so we record that we saw block 2.  We see that
 * bc_ptrs[1] == 2, so we exit the loop.  The list is now [1, 4, 2].
 *
 * For the 102nd record, bc_ptrs[0] == 2, so we continue.
 *
 * For the 201st record, we've moved on to leaf block 3.  bc_ptrs[0] == 1, so
 * we add 3 to the list.  Now it is [1, 4, 2, 3].
 *
 * For the 300th record we just exit, with the list being [1, 4, 2, 3].
 */

/*
 * Record all the buffers pointed to by the btree cursor.  Callers already
 * engaged in a btree walk should call this function to capture the list of
 * blocks going from the leaf towards the root.
 */
int
xbitmap_set_btcur_path(
	struct xbitmap		*bitmap,
	struct xfs_btree_cur	*cur)
{
	struct xfs_buf		*bp;
	xfs_fsblock_t		fsb;
	int			i;
	int			error;

	for (i = 0; i < cur->bc_nlevels && cur->bc_ptrs[i] == 1; i++) {
		xfs_btree_get_block(cur, i, &bp);
		if (!bp)
			continue;
		fsb = XFS_DADDR_TO_FSB(cur->bc_mp, bp->b_bn);
		error = xbitmap_set(bitmap, fsb, 1);
		if (error)
			return error;
	}

	return 0;
}

/* Collect a btree's block in the bitmap. */
STATIC int
xbitmap_collect_btblock(
	struct xfs_btree_cur	*cur,
	int			level,
	void			*priv)
{
	struct xbitmap		*bitmap = priv;
	struct xfs_buf		*bp;
	xfs_fsblock_t		fsbno;

	xfs_btree_get_block(cur, level, &bp);
	if (!bp)
		return 0;

	fsbno = XFS_DADDR_TO_FSB(cur->bc_mp, bp->b_bn);
	return xbitmap_set(bitmap, fsbno, 1);
}

/* Walk the btree and mark the bitmap wherever a btree block is found. */
int
xbitmap_set_btblocks(
	struct xbitmap		*bitmap,
	struct xfs_btree_cur	*cur)
{
	return xfs_btree_visit_blocks(cur, xbitmap_collect_btblock,
			XFS_BTREE_VISIT_ALL, bitmap);
}

/* How many bits are set in this bitmap? */
uint64_t
xbitmap_hweight(
	struct xbitmap			*bitmap)
{
	struct interval_tree_node	*itn;
	uint64_t			ret = 0;

	for_each_xbitmap_extent(itn, bitmap)
		ret += itn->last - itn->start + 1;

	return ret;
}

/* Call a function for every run of set bits in this bitmap. */
int
xbitmap_walk(
	struct xbitmap			*bitmap,
	xbitmap_walk_fn		fn,
	void				*priv)
{
	struct interval_tree_node	*itn;
	int				error;

	for_each_xbitmap_extent(itn, bitmap) {
		error = fn(itn->start, itn->last - itn->start + 1, priv);
		if (error)
			break;
	}

	return error;
}

struct xbitmap_walk_bits {
	xbitmap_walk_bits_fn	fn;
	void			*priv;
};

/* Walk all the bits in a run. */
static int
xbitmap_walk_bits_in_run(
	uint64_t			start,
	uint64_t			len,
	void				*priv)
{
	struct xbitmap_walk_bits	*wb = priv;
	uint64_t			i;
	int				error;

	for (i = start; i < start + len; i++) {
		error = wb->fn(i, wb->priv);
		if (error)
			break;
	}

	return error;
}

/* Call a function for every set bit in this bitmap. */
int
xbitmap_walk_bits(
	struct xbitmap			*bitmap,
	xbitmap_walk_bits_fn		fn,
	void				*priv)
{
	struct xbitmap_walk_bits	wb = {.fn = fn, .priv = priv};

	return xbitmap_walk(bitmap, xbitmap_walk_bits_in_run, &wb);
}

/* Does this bitmap have no bits set at all? */
bool
xbitmap_empty(
	struct xbitmap		*bitmap)
{
	return bitmap->root.rb_root.rb_node == NULL;
}
