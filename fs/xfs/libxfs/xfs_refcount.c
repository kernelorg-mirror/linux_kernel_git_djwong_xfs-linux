// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2016 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_btree.h"
#include "xfs_bmap.h"
#include "xfs_refcount_btree.h"
#include "xfs_alloc.h"
#include "xfs_errortag.h"
#include "xfs_error.h"
#include "xfs_trace.h"
#include "xfs_trans.h"
#include "xfs_bit.h"
#include "xfs_refcount.h"
#include "xfs_rmap.h"
#include "xfs_health.h"

/* Allowable refcount adjustment amounts. */
enum xfs_refc_adjust_op {
	XFS_REFCOUNT_ADJUST_INCREASE	= 1,
	XFS_REFCOUNT_ADJUST_DECREASE	= -1,
	XFS_REFCOUNT_ADJUST_COW_ALLOC	= 0,
	XFS_REFCOUNT_ADJUST_COW_FREE	= -1,
};

STATIC int __xfs_refcount_cow_alloc(struct xfs_btree_cur *rcur,
		xfs_fsblock_t bno, xfs_filblks_t len);
STATIC int __xfs_refcount_cow_free(struct xfs_btree_cur *rcur,
		xfs_fsblock_t bno, xfs_filblks_t len);

/* By convention, the rtrefcountbt's "AG" number is NULLAGNUMBER. */
static xfs_agnumber_t
xrefc_cur_agno(
	struct xfs_btree_cur	*cur)
{
	return cur->bc_btnum == XFS_BTNUM_RTREFC ?
			NULLAGNUMBER : cur->bc_ag.agno;
}

/* Return the maximum length of a refcount record. */
static xfs_filblks_t
xrefc_max_rec_len(
	struct xfs_btree_cur	*cur)
{
	return cur->bc_btnum == XFS_BTNUM_RTREFC ?
			MAXRTREFCEXTLEN : MAXREFCEXTLEN;
}

/* Return the COW start flag. */
static xfs_filblks_t
xrefc_cow_start(
	struct xfs_btree_cur	*cur)
{
	return cur->bc_btnum == XFS_BTNUM_RTREFC ?
			XFS_RTREFC_COW_START : XFS_REFC_COW_START;
}

/* Return the maximum startblock number of the refcountbt. */
static xfs_fsblock_t
xrefc_max_startblock(
	struct xfs_btree_cur	*cur)
{
	return cur->bc_btnum == XFS_BTNUM_RTREFC ?
			cur->bc_mp->m_sb.sb_rblocks :
			cur->bc_mp->m_sb.sb_agblocks;
}

/*
 * Look up the first record less than or equal to [bno, len] in the btree
 * given by cur.
 */
int
xfs_refcount_lookup_le(
	struct xfs_btree_cur	*cur,
	xfs_fsblock_t		bno,
	int			*stat)
{
	trace_xfs_refcount_lookup(cur->bc_mp, xrefc_cur_agno(cur), bno,
			XFS_LOOKUP_LE);
	cur->bc_rec.rc.rc_startblock = bno;
	cur->bc_rec.rc.rc_blockcount = 0;
	return xfs_btree_lookup(cur, XFS_LOOKUP_LE, stat);
}

/*
 * Look up the first record greater than or equal to [bno, len] in the btree
 * given by cur.
 */
int
xfs_refcount_lookup_ge(
	struct xfs_btree_cur	*cur,
	xfs_fsblock_t		bno,
	int			*stat)
{
	trace_xfs_refcount_lookup(cur->bc_mp, xrefc_cur_agno(cur), bno,
			XFS_LOOKUP_GE);
	cur->bc_rec.rc.rc_startblock = bno;
	cur->bc_rec.rc.rc_blockcount = 0;
	return xfs_btree_lookup(cur, XFS_LOOKUP_GE, stat);
}

/*
 * Look up the first record equal to [bno, len] in the btree
 * given by cur.
 */
int
xfs_refcount_lookup_eq(
	struct xfs_btree_cur	*cur,
	xfs_fsblock_t		bno,
	int			*stat)
{
	trace_xfs_refcount_lookup(cur->bc_mp, xrefc_cur_agno(cur), bno,
			XFS_LOOKUP_LE);
	cur->bc_rec.rc.rc_startblock = bno;
	cur->bc_rec.rc.rc_blockcount = 0;
	return xfs_btree_lookup(cur, XFS_LOOKUP_EQ, stat);
}

/* Convert on-disk record to in-core format. */
void
xfs_refcount_btrec_to_irec(
	struct xfs_btree_cur		*cur,
	union xfs_btree_rec		*rec,
	struct xfs_refcount_irec	*irec)
{
	if (cur->bc_flags & XFS_BTREE_LONG_PTRS) {
		irec->rc_startblock = be64_to_cpu(rec->rtrefc.rc_startblock);
		irec->rc_blockcount = be64_to_cpu(rec->rtrefc.rc_blockcount);
		irec->rc_refcount = be32_to_cpu(rec->rtrefc.rc_refcount);
	} else {
		irec->rc_startblock = be32_to_cpu(rec->refc.rc_startblock);
		irec->rc_blockcount = be32_to_cpu(rec->refc.rc_blockcount);
		irec->rc_refcount = be32_to_cpu(rec->refc.rc_refcount);
	}
}

/*
 * Get the data from the pointed-to record.
 */
int
xfs_refcount_get_rec(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*irec,
	int				*stat)
{
	struct xfs_mount		*mp = cur->bc_mp;
	xfs_agnumber_t			agno = xrefc_cur_agno(cur);
	union xfs_btree_rec		*rec;
	int				error;
	xfs_fsblock_t			realstart;
	xfs_fsblock_t			cow_start;

	error = xfs_btree_get_rec(cur, &rec, stat);
	if (error || !*stat)
		return error;

	xfs_refcount_btrec_to_irec(cur, rec, irec);

	agno = xrefc_cur_agno(cur);
	if (irec->rc_blockcount == 0 ||
	    irec->rc_blockcount > xrefc_max_rec_len(cur))
		goto out_bad_rec;

	/* handle special COW-staging state */
	realstart = irec->rc_startblock;
	cow_start = xrefc_cow_start(cur);
	if (realstart & cow_start) {
		if (irec->rc_refcount != 1)
			goto out_bad_rec;
		realstart &= ~cow_start;
	} else if (irec->rc_refcount < 2) {
		goto out_bad_rec;
	}

	/* check for valid extent range, including overflow */
	if (cur->bc_btnum == XFS_BTNUM_RTREFC) {
		if (!xfs_verify_rtbno(mp, realstart))
			goto out_bad_rec;
		if (realstart > realstart + irec->rc_blockcount)
			goto out_bad_rec;
		if (!xfs_verify_rtbno(mp, realstart + irec->rc_blockcount - 1))
			goto out_bad_rec;
	} else {
		if (!xfs_verify_agbno(mp, agno, realstart))
			goto out_bad_rec;
		if (realstart > realstart + irec->rc_blockcount)
			goto out_bad_rec;
		if (!xfs_verify_agbno(mp, agno,
				realstart + irec->rc_blockcount - 1))
			goto out_bad_rec;
	}

	if (irec->rc_refcount == 0 || irec->rc_refcount > MAXREFCOUNT)
		goto out_bad_rec;

	trace_xfs_refcount_get(cur->bc_mp, agno, irec);
	return 0;

out_bad_rec:
	xfs_warn(mp,
		"Refcount BTree record corruption in AG %d detected!", agno);
	xfs_warn(mp,
		"Start block 0x%llx, block count 0x%llx, references 0x%x",
		irec->rc_startblock, irec->rc_blockcount, irec->rc_refcount);
	xfs_btree_mark_sick(cur);
	return -EFSCORRUPTED;
}

/*
 * Update the record referred to by cur to the value given
 * by [bno, len, refcount].
 * This either works (return 0) or gets an EFSCORRUPTED error.
 */
STATIC int
xfs_refcount_update(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*irec)
{
	union xfs_btree_rec	rec;
	int			error;

	trace_xfs_refcount_update(cur->bc_mp, xrefc_cur_agno(cur), irec);

	if (cur->bc_btnum == XFS_BTNUM_RTREFC) {
		rec.rtrefc.rc_startblock = cpu_to_be64(irec->rc_startblock);
		rec.rtrefc.rc_blockcount = cpu_to_be64(irec->rc_blockcount);
		rec.rtrefc.rc_refcount = cpu_to_be32(irec->rc_refcount);
	} else {
		rec.refc.rc_startblock = cpu_to_be32(irec->rc_startblock);
		rec.refc.rc_blockcount = cpu_to_be32(irec->rc_blockcount);
		rec.refc.rc_refcount = cpu_to_be32(irec->rc_refcount);
	}
	error = xfs_btree_update(cur, &rec);
	if (error)
		trace_xfs_refcount_update_error(cur->bc_mp,
				xrefc_cur_agno(cur), error, _RET_IP_);
	return error;
}

/*
 * Insert the record referred to by cur to the value given
 * by [bno, len, refcount].
 * This either works (return 0) or gets an EFSCORRUPTED error.
 */
int
xfs_refcount_insert(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*irec,
	int				*i)
{
	int				error;

	trace_xfs_refcount_insert(cur->bc_mp, xrefc_cur_agno(cur), irec);
	cur->bc_rec.rc.rc_startblock = irec->rc_startblock;
	cur->bc_rec.rc.rc_blockcount = irec->rc_blockcount;
	cur->bc_rec.rc.rc_refcount = irec->rc_refcount;
	error = xfs_btree_insert(cur, i);
	if (error)
		goto out_error;
	if (XFS_IS_CORRUPT(cur->bc_mp, *i != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}

out_error:
	if (error)
		trace_xfs_refcount_insert_error(cur->bc_mp,
				xrefc_cur_agno(cur), error, _RET_IP_);
	return error;
}

/*
 * Remove the record referred to by cur, then set the pointer to the spot
 * where the record could be re-inserted, in case we want to increment or
 * decrement the cursor.
 * This either works (return 0) or gets an EFSCORRUPTED error.
 */
STATIC int
xfs_refcount_delete(
	struct xfs_btree_cur	*cur,
	int			*i)
{
	struct xfs_refcount_irec	irec;
	int			found_rec;
	int			error;

	error = xfs_refcount_get_rec(cur, &irec, &found_rec);
	if (error)
		goto out_error;
	if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}
	trace_xfs_refcount_delete(cur->bc_mp, xrefc_cur_agno(cur), &irec);
	error = xfs_btree_delete(cur, i);
	if (XFS_IS_CORRUPT(cur->bc_mp, *i != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}
	if (error)
		goto out_error;
	error = xfs_refcount_lookup_ge(cur, irec.rc_startblock, &found_rec);
out_error:
	if (error)
		trace_xfs_refcount_delete_error(cur->bc_mp,
				xrefc_cur_agno(cur), error, _RET_IP_);
	return error;
}

/*
 * Adjusting the Reference Count
 *
 * As stated elsewhere, the reference count btree (refcbt) stores
 * >1 reference counts for extents of physical blocks.  In this
 * operation, we're either raising or lowering the reference count of
 * some subrange stored in the tree:
 *
 *      <------ adjustment range ------>
 * ----+   +---+-----+ +--+--------+---------
 *  2  |   | 3 |  4  | |17|   55   |   10
 * ----+   +---+-----+ +--+--------+---------
 * X axis is physical blocks number;
 * reference counts are the numbers inside the rectangles
 *
 * The first thing we need to do is to ensure that there are no
 * refcount extents crossing either boundary of the range to be
 * adjusted.  For any extent that does cross a boundary, split it into
 * two extents so that we can increment the refcount of one of the
 * pieces later:
 *
 *      <------ adjustment range ------>
 * ----+   +---+-----+ +--+--------+----+----
 *  2  |   | 3 |  2  | |17|   55   | 10 | 10
 * ----+   +---+-----+ +--+--------+----+----
 *
 * For this next step, let's assume that all the physical blocks in
 * the adjustment range are mapped to a file and are therefore in use
 * at least once.  Therefore, we can infer that any gap in the
 * refcount tree within the adjustment range represents a physical
 * extent with refcount == 1:
 *
 *      <------ adjustment range ------>
 * ----+---+---+-----+-+--+--------+----+----
 *  2  |"1"| 3 |  2  |1|17|   55   | 10 | 10
 * ----+---+---+-----+-+--+--------+----+----
 *      ^
 *
 * For each extent that falls within the interval range, figure out
 * which extent is to the left or the right of that extent.  Now we
 * have a left, current, and right extent.  If the new reference count
 * of the center extent enables us to merge left, center, and right
 * into one record covering all three, do so.  If the center extent is
 * at the left end of the range, abuts the left extent, and its new
 * reference count matches the left extent's record, then merge them.
 * If the center extent is at the right end of the range, abuts the
 * right extent, and the reference counts match, merge those.  In the
 * example, we can left merge (assuming an increment operation):
 *
 *      <------ adjustment range ------>
 * --------+---+-----+-+--+--------+----+----
 *    2    | 3 |  2  |1|17|   55   | 10 | 10
 * --------+---+-----+-+--+--------+----+----
 *          ^
 *
 * For all other extents within the range, adjust the reference count
 * or delete it if the refcount falls below 2.  If we were
 * incrementing, the end result looks like this:
 *
 *      <------ adjustment range ------>
 * --------+---+-----+-+--+--------+----+----
 *    2    | 4 |  3  |2|18|   56   | 11 | 10
 * --------+---+-----+-+--+--------+----+----
 *
 * The result of a decrement operation looks as such:
 *
 *      <------ adjustment range ------>
 * ----+   +---+       +--+--------+----+----
 *  2  |   | 2 |       |16|   54   |  9 | 10
 * ----+   +---+       +--+--------+----+----
 *      DDDD    111111DD
 *
 * The blocks marked "D" are freed; the blocks marked "1" are only
 * referenced once and therefore the record is removed from the
 * refcount btree.
 */

/* Next block after this extent. */
static inline xfs_fsblock_t
xfs_refc_next(
	struct xfs_refcount_irec	*rc)
{
	return rc->rc_startblock + rc->rc_blockcount;
}

/*
 * Split a refcount extent that crosses bno.
 */
STATIC int
xfs_refcount_split_extent(
	struct xfs_btree_cur		*cur,
	xfs_fsblock_t			bno,
	bool				*shape_changed)
{
	struct xfs_refcount_irec	rcext, tmp;
	int				found_rec;
	int				error;

	*shape_changed = false;
	error = xfs_refcount_lookup_le(cur, bno, &found_rec);
	if (error)
		goto out_error;
	if (!found_rec)
		return 0;

	error = xfs_refcount_get_rec(cur, &rcext, &found_rec);
	if (error)
		goto out_error;
	if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}
	if (rcext.rc_startblock == bno || xfs_refc_next(&rcext) <= bno)
		return 0;

	*shape_changed = true;
	trace_xfs_refcount_split_extent(cur->bc_mp, xrefc_cur_agno(cur),
			&rcext, bno);

	/* Establish the right extent. */
	tmp = rcext;
	tmp.rc_startblock = bno;
	tmp.rc_blockcount -= (bno - rcext.rc_startblock);
	error = xfs_refcount_update(cur, &tmp);
	if (error)
		goto out_error;

	/* Insert the left extent. */
	tmp = rcext;
	tmp.rc_blockcount = bno - rcext.rc_startblock;
	error = xfs_refcount_insert(cur, &tmp, &found_rec);
	if (error)
		goto out_error;
	if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}
	return error;

out_error:
	trace_xfs_refcount_split_extent_error(cur->bc_mp,
			xrefc_cur_agno(cur), error, _RET_IP_);
	return error;
}

/*
 * Merge the left, center, and right extents.
 */
STATIC int
xfs_refcount_merge_center_extents(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*left,
	struct xfs_refcount_irec	*center,
	struct xfs_refcount_irec	*right,
	unsigned long long		extlen,
	xfs_filblks_t			*len)
{
	int				error;
	int				found_rec;

	trace_xfs_refcount_merge_center_extents(cur->bc_mp,
			xrefc_cur_agno(cur), left, center, right);

	/*
	 * Make sure the center and right extents are not in the btree.
	 * If the center extent was synthesized, the first delete call
	 * removes the right extent and we skip the second deletion.
	 * If center and right were in the btree, then the first delete
	 * call removes the center and the second one removes the right
	 * extent.
	 */
	error = xfs_refcount_lookup_ge(cur, center->rc_startblock,
			&found_rec);
	if (error)
		goto out_error;
	if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}

	error = xfs_refcount_delete(cur, &found_rec);
	if (error)
		goto out_error;
	if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}

	if (center->rc_refcount > 1) {
		error = xfs_refcount_delete(cur, &found_rec);
		if (error)
			goto out_error;
		if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}
	}

	/* Enlarge the left extent. */
	error = xfs_refcount_lookup_le(cur, left->rc_startblock,
			&found_rec);
	if (error)
		goto out_error;
	if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}

	left->rc_blockcount = extlen;
	error = xfs_refcount_update(cur, left);
	if (error)
		goto out_error;

	*len = 0;
	return error;

out_error:
	trace_xfs_refcount_merge_center_extents_error(cur->bc_mp,
			xrefc_cur_agno(cur), error, _RET_IP_);
	return error;
}

/*
 * Merge with the left extent.
 */
STATIC int
xfs_refcount_merge_left_extent(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*left,
	struct xfs_refcount_irec	*cleft,
	xfs_fsblock_t			*bno,
	xfs_filblks_t			*len)
{
	int				error;
	int				found_rec;

	trace_xfs_refcount_merge_left_extent(cur->bc_mp,
			xrefc_cur_agno(cur), left, cleft);

	/* If the extent at bno (cleft) wasn't synthesized, remove it. */
	if (cleft->rc_refcount > 1) {
		error = xfs_refcount_lookup_le(cur, cleft->rc_startblock,
				&found_rec);
		if (error)
			goto out_error;
		if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}

		error = xfs_refcount_delete(cur, &found_rec);
		if (error)
			goto out_error;
		if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}
	}

	/* Enlarge the left extent. */
	error = xfs_refcount_lookup_le(cur, left->rc_startblock,
			&found_rec);
	if (error)
		goto out_error;
	if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}

	left->rc_blockcount += cleft->rc_blockcount;
	error = xfs_refcount_update(cur, left);
	if (error)
		goto out_error;

	*bno += cleft->rc_blockcount;
	*len -= cleft->rc_blockcount;
	return error;

out_error:
	trace_xfs_refcount_merge_left_extent_error(cur->bc_mp,
			xrefc_cur_agno(cur), error, _RET_IP_);
	return error;
}

/*
 * Merge with the right extent.
 */
STATIC int
xfs_refcount_merge_right_extent(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*right,
	struct xfs_refcount_irec	*cright,
	xfs_filblks_t			*len)
{
	int				error;
	int				found_rec;

	trace_xfs_refcount_merge_right_extent(cur->bc_mp,
			xrefc_cur_agno(cur), cright, right);

	/*
	 * If the extent ending at bno+len (cright) wasn't synthesized,
	 * remove it.
	 */
	if (cright->rc_refcount > 1) {
		error = xfs_refcount_lookup_le(cur, cright->rc_startblock,
			&found_rec);
		if (error)
			goto out_error;
		if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}

		error = xfs_refcount_delete(cur, &found_rec);
		if (error)
			goto out_error;
		if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}
	}

	/* Enlarge the right extent. */
	error = xfs_refcount_lookup_le(cur, right->rc_startblock,
			&found_rec);
	if (error)
		goto out_error;
	if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}

	right->rc_startblock -= cright->rc_blockcount;
	right->rc_blockcount += cright->rc_blockcount;
	error = xfs_refcount_update(cur, right);
	if (error)
		goto out_error;

	*len -= cright->rc_blockcount;
	return error;

out_error:
	trace_xfs_refcount_merge_right_extent_error(cur->bc_mp,
			xrefc_cur_agno(cur), error, _RET_IP_);
	return error;
}

#define XFS_FIND_RCEXT_SHARED	1
#define XFS_FIND_RCEXT_COW	2
/*
 * Find the left extent and the one after it (cleft).  This function assumes
 * that we've already split any extent crossing bno.
 */
STATIC int
xfs_refcount_find_left_extents(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*left,
	struct xfs_refcount_irec	*cleft,
	xfs_fsblock_t			bno,
	xfs_filblks_t			len,
	int				flags)
{
	struct xfs_refcount_irec	tmp;
	int				error;
	int				found_rec;

	left->rc_startblock = cleft->rc_startblock = NULLFSBLOCK;
	error = xfs_refcount_lookup_le(cur, bno - 1, &found_rec);
	if (error)
		goto out_error;
	if (!found_rec)
		return 0;

	error = xfs_refcount_get_rec(cur, &tmp, &found_rec);
	if (error)
		goto out_error;
	if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}

	if (xfs_refc_next(&tmp) != bno)
		return 0;
	if ((flags & XFS_FIND_RCEXT_SHARED) && tmp.rc_refcount < 2)
		return 0;
	if ((flags & XFS_FIND_RCEXT_COW) && tmp.rc_refcount > 1)
		return 0;
	/* We have a left extent; retrieve (or invent) the next right one */
	*left = tmp;

	error = xfs_btree_increment(cur, 0, &found_rec);
	if (error)
		goto out_error;
	if (found_rec) {
		error = xfs_refcount_get_rec(cur, &tmp, &found_rec);
		if (error)
			goto out_error;
		if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}

		/* if tmp starts at the end of our range, just use that */
		if (tmp.rc_startblock == bno)
			*cleft = tmp;
		else {
			/*
			 * There's a gap in the refcntbt at the start of the
			 * range we're interested in (refcount == 1) so
			 * synthesize the implied extent and pass it back.
			 * We assume here that the bno/len range was
			 * passed in from a data fork extent mapping and
			 * therefore is allocated to exactly one owner.
			 */
			cleft->rc_startblock = bno;
			cleft->rc_blockcount = min(len,
					tmp.rc_startblock - bno);
			cleft->rc_refcount = 1;
		}
	} else {
		/*
		 * No extents, so pretend that there's one covering the whole
		 * range.
		 */
		cleft->rc_startblock = bno;
		cleft->rc_blockcount = len;
		cleft->rc_refcount = 1;
	}
	trace_xfs_refcount_find_left_extent(cur->bc_mp, xrefc_cur_agno(cur),
			left, cleft, bno);
	return error;

out_error:
	trace_xfs_refcount_find_left_extent_error(cur->bc_mp,
			xrefc_cur_agno(cur), error, _RET_IP_);
	return error;
}

/*
 * Find the right extent and the one before it (cright).  This function
 * assumes that we've already split any extents crossing bno + len.
 */
STATIC int
xfs_refcount_find_right_extents(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*right,
	struct xfs_refcount_irec	*cright,
	xfs_fsblock_t			bno,
	xfs_filblks_t			len,
	int				flags)
{
	struct xfs_refcount_irec	tmp;
	int				error;
	int				found_rec;

	right->rc_startblock = cright->rc_startblock = NULLFSBLOCK;
	error = xfs_refcount_lookup_ge(cur, bno + len, &found_rec);
	if (error)
		goto out_error;
	if (!found_rec)
		return 0;

	error = xfs_refcount_get_rec(cur, &tmp, &found_rec);
	if (error)
		goto out_error;
	if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}

	if (tmp.rc_startblock != bno + len)
		return 0;
	if ((flags & XFS_FIND_RCEXT_SHARED) && tmp.rc_refcount < 2)
		return 0;
	if ((flags & XFS_FIND_RCEXT_COW) && tmp.rc_refcount > 1)
		return 0;
	/* We have a right extent; retrieve (or invent) the next left one */
	*right = tmp;

	error = xfs_btree_decrement(cur, 0, &found_rec);
	if (error)
		goto out_error;
	if (found_rec) {
		error = xfs_refcount_get_rec(cur, &tmp, &found_rec);
		if (error)
			goto out_error;
		if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}

		/* if tmp ends at the end of our range, just use that */
		if (xfs_refc_next(&tmp) == bno + len)
			*cright = tmp;
		else {
			/*
			 * There's a gap in the refcntbt at the end of the
			 * range we're interested in (refcount == 1) so
			 * create the implied extent and pass it back.
			 * We assume here that the bno/len range was
			 * passed in from a data fork extent mapping and
			 * therefore is allocated to exactly one owner.
			 */
			cright->rc_startblock = max(bno, xfs_refc_next(&tmp));
			cright->rc_blockcount = right->rc_startblock -
					cright->rc_startblock;
			cright->rc_refcount = 1;
		}
	} else {
		/*
		 * No extents, so pretend that there's one covering the whole
		 * range.
		 */
		cright->rc_startblock = bno;
		cright->rc_blockcount = len;
		cright->rc_refcount = 1;
	}
	trace_xfs_refcount_find_right_extent(cur->bc_mp, xrefc_cur_agno(cur),
			cright, right, bno + len);
	return error;

out_error:
	trace_xfs_refcount_find_right_extent_error(cur->bc_mp,
			xrefc_cur_agno(cur), error, _RET_IP_);
	return error;
}

/* Is this extent valid? */
static inline bool
xfs_refc_valid(
	struct xfs_refcount_irec	*rc)
{
	return rc->rc_startblock != NULLFSBLOCK;
}

/*
 * Try to merge with any extents on the boundaries of the adjustment range.
 */
STATIC int
xfs_refcount_merge_extents(
	struct xfs_btree_cur	*cur,
	xfs_fsblock_t		*bno,
	xfs_filblks_t		*len,
	enum xfs_refc_adjust_op adjust,
	int			flags,
	bool			*shape_changed)
{
	struct xfs_refcount_irec	left = {0}, cleft = {0};
	struct xfs_refcount_irec	cright = {0}, right = {0};
	int				error;
	unsigned long long		ulen;
	bool				cequal;

	*shape_changed = false;
	/*
	 * Find the extent just below bno [left], just above bno [cleft],
	 * just below (bno + len) [cright], and just above (bno + len)
	 * [right].
	 */
	error = xfs_refcount_find_left_extents(cur, &left, &cleft, *bno,
			*len, flags);
	if (error)
		return error;
	error = xfs_refcount_find_right_extents(cur, &right, &cright, *bno,
			*len, flags);
	if (error)
		return error;

	/* No left or right extent to merge; exit. */
	if (!xfs_refc_valid(&left) && !xfs_refc_valid(&right))
		return 0;

	cequal = (cleft.rc_startblock == cright.rc_startblock) &&
		 (cleft.rc_blockcount == cright.rc_blockcount);

	/* Try to merge left, cleft, and right.  cleft must == cright. */
	ulen = (unsigned long long)left.rc_blockcount + cleft.rc_blockcount +
			right.rc_blockcount;
	if (xfs_refc_valid(&left) && xfs_refc_valid(&right) &&
	    xfs_refc_valid(&cleft) && xfs_refc_valid(&cright) && cequal &&
	    left.rc_refcount == cleft.rc_refcount + adjust &&
	    right.rc_refcount == cleft.rc_refcount + adjust &&
	    ulen < xrefc_max_rec_len(cur)) {
		*shape_changed = true;
		return xfs_refcount_merge_center_extents(cur, &left, &cleft,
				&right, ulen, len);
	}

	/* Try to merge left and cleft. */
	ulen = (unsigned long long)left.rc_blockcount + cleft.rc_blockcount;
	if (xfs_refc_valid(&left) && xfs_refc_valid(&cleft) &&
	    left.rc_refcount == cleft.rc_refcount + adjust &&
	    ulen < xrefc_max_rec_len(cur)) {
		*shape_changed = true;
		error = xfs_refcount_merge_left_extent(cur, &left, &cleft,
				bno, len);
		if (error)
			return error;

		/*
		 * If we just merged left + cleft and cleft == cright,
		 * we no longer have a cright to merge with right.  We're done.
		 */
		if (cequal)
			return 0;
	}

	/* Try to merge cright and right. */
	ulen = (unsigned long long)right.rc_blockcount + cright.rc_blockcount;
	if (xfs_refc_valid(&right) && xfs_refc_valid(&cright) &&
	    right.rc_refcount == cright.rc_refcount + adjust &&
	    ulen < xrefc_max_rec_len(cur)) {
		*shape_changed = true;
		return xfs_refcount_merge_right_extent(cur, &right, &cright,
				len);
	}

	return error;
}

static inline struct xbtree_refc *
xrefc_btree_state(
	struct xfs_btree_cur	*cur)
{
	if (cur->bc_btnum == XFS_BTNUM_RTREFC)
		return &cur->bc_ino.refc;
	return &cur->bc_ag.refc;
}

/*
 * XXX: This is a pretty hand-wavy estimate.  The penalty for guessing
 * true incorrectly is a shutdown FS; the penalty for guessing false
 * incorrectly is more transaction rolls than might be necessary.
 * Be conservative here.
 */
static bool
xfs_refcount_still_have_space(
	struct xfs_btree_cur		*cur)
{
	unsigned long			overhead;

	/*
	 * Worst case estimate: full splits of the free space and rmap btrees
	 * to handle each of the shape changes to the refcount btree.
	 */
	overhead = xfs_allocfree_log_count(cur->bc_mp,
				xrefc_btree_state(cur)->shape_changes);
	overhead += cur->bc_maxlevels;
	overhead *= cur->bc_mp->m_sb.sb_blocksize;

	/*
	 * Only allow 2 refcount extent updates per transaction if the
	 * refcount continue update "error" has been injected.
	 */
	if (xrefc_btree_state(cur)->nr_ops > 2 &&
	    XFS_TEST_ERROR(false, cur->bc_mp,
			XFS_ERRTAG_REFCOUNT_CONTINUE_UPDATE))
		return false;

	if (xrefc_btree_state(cur)->nr_ops == 0)
		return true;
	else if (overhead > cur->bc_tp->t_log_res)
		return false;
	return  cur->bc_tp->t_log_res - overhead >
		xrefc_btree_state(cur)->nr_ops * XFS_REFCOUNT_ITEM_OVERHEAD;
}

/*
 * Adjust the refcounts of middle extents.  At this point we should have
 * split extents that crossed the adjustment range; merged with adjacent
 * extents; and updated bno/len to reflect the merges.  Therefore,
 * all we have to do is update the extents inside [bno, bno + len].
 */
STATIC int
xfs_refcount_adjust_extents(
	struct xfs_btree_cur	*cur,
	xfs_fsblock_t		*bno,
	xfs_filblks_t		*len,
	enum xfs_refc_adjust_op	adj)
{
	struct xfs_refcount_irec	ext, tmp;
	int				error;
	int				found_rec, found_tmp;
	xfs_fsblock_t			fsbno;

	/* Merging did all the work already. */
	if (*len == 0)
		return 0;

	error = xfs_refcount_lookup_ge(cur, *bno, &found_rec);
	if (error)
		goto out_error;

	while (*len > 0 && xfs_refcount_still_have_space(cur)) {
		error = xfs_refcount_get_rec(cur, &ext, &found_rec);
		if (error)
			goto out_error;
		if (!found_rec) {
			ext.rc_startblock = xrefc_max_startblock(cur);
			ext.rc_blockcount = 0;
			ext.rc_refcount = 0;
		}

		/*
		 * Deal with a hole in the refcount tree; if a file maps to
		 * these blocks and there's no refcountbt record, pretend that
		 * there is one with refcount == 1.
		 */
		if (ext.rc_startblock != *bno) {
			tmp.rc_startblock = *bno;
			tmp.rc_blockcount = min(*len,
					ext.rc_startblock - *bno);
			tmp.rc_refcount = 1 + adj;
			trace_xfs_refcount_modify_extent(cur->bc_mp,
					xrefc_cur_agno(cur), &tmp);

			/*
			 * Either cover the hole (increment) or
			 * delete the range (decrement).
			 */
			xrefc_btree_state(cur)->nr_ops++;
			if (tmp.rc_refcount) {
				error = xfs_refcount_insert(cur, &tmp,
						&found_tmp);
				if (error)
					goto out_error;
				if (XFS_IS_CORRUPT(cur->bc_mp,
						   found_tmp != 1)) {
					xfs_btree_mark_sick(cur);
					error = -EFSCORRUPTED;
					goto out_error;
				}
			} else {
				fsbno = XFS_AGB_TO_FSB(cur->bc_mp,
						xrefc_cur_agno(cur),
						tmp.rc_startblock);
				xfs_bmap_add_free(cur->bc_tp, fsbno,
						  tmp.rc_blockcount, NULL,
						  false);
			}

			(*bno) += tmp.rc_blockcount;
			(*len) -= tmp.rc_blockcount;

			error = xfs_refcount_lookup_ge(cur, *bno,
					&found_rec);
			if (error)
				goto out_error;
		}

		/* Stop if there's nothing left to modify */
		if (*len == 0 || !xfs_refcount_still_have_space(cur))
			break;

		/*
		 * Adjust the reference count and either update the tree
		 * (incr) or free the blocks (decr).
		 */
		if (ext.rc_refcount == MAXREFCOUNT)
			goto skip;
		ext.rc_refcount += adj;
		trace_xfs_refcount_modify_extent(cur->bc_mp,
				xrefc_cur_agno(cur), &ext);
		xrefc_btree_state(cur)->nr_ops++;
		if (ext.rc_refcount > 1) {
			error = xfs_refcount_update(cur, &ext);
			if (error)
				goto out_error;
		} else if (ext.rc_refcount == 1) {
			error = xfs_refcount_delete(cur, &found_rec);
			if (error)
				goto out_error;
			if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto out_error;
			}
			goto advloop;
		} else {
			fsbno = XFS_AGB_TO_FSB(cur->bc_mp,
					xrefc_cur_agno(cur),
					ext.rc_startblock);
			xfs_bmap_add_free(cur->bc_tp, fsbno, ext.rc_blockcount,
					  NULL, false);
		}

skip:
		error = xfs_btree_increment(cur, 0, &found_rec);
		if (error)
			goto out_error;

advloop:
		(*bno) += ext.rc_blockcount;
		(*len) -= ext.rc_blockcount;
	}

	return error;
out_error:
	trace_xfs_refcount_modify_extent_error(cur->bc_mp,
			xrefc_cur_agno(cur), error, _RET_IP_);
	return error;
}

/* Adjust the reference count of a range of AG blocks. */
STATIC int
xfs_refcount_adjust(
	struct xfs_btree_cur	*cur,
	xfs_fsblock_t		bno,
	xfs_filblks_t		len,
	xfs_fsblock_t		*new_bno,
	xfs_filblks_t		*new_len,
	enum xfs_refc_adjust_op	adj)
{
	bool			shape_changed;
	int			shape_changes = 0;
	int			error;

	*new_bno = bno;
	*new_len = len;
	if (adj == XFS_REFCOUNT_ADJUST_INCREASE)
		trace_xfs_refcount_increase(cur->bc_mp, xrefc_cur_agno(cur),
				bno, len);
	else
		trace_xfs_refcount_decrease(cur->bc_mp, xrefc_cur_agno(cur),
				bno, len);

	/*
	 * Ensure that no rcextents cross the boundary of the adjustment range.
	 */
	error = xfs_refcount_split_extent(cur, bno, &shape_changed);
	if (error)
		goto out_error;
	if (shape_changed)
		shape_changes++;

	error = xfs_refcount_split_extent(cur, bno + len, &shape_changed);
	if (error)
		goto out_error;
	if (shape_changed)
		shape_changes++;

	/*
	 * Try to merge with the left or right extents of the range.
	 */
	error = xfs_refcount_merge_extents(cur, new_bno, new_len, adj,
			XFS_FIND_RCEXT_SHARED, &shape_changed);
	if (error)
		goto out_error;
	if (shape_changed)
		shape_changes++;
	if (shape_changes)
		xrefc_btree_state(cur)->shape_changes++;

	/* Now that we've taken care of the ends, adjust the middle extents */
	error = xfs_refcount_adjust_extents(cur, new_bno, new_len, adj);
	if (error)
		goto out_error;

	return 0;

out_error:
	trace_xfs_refcount_adjust_error(cur->bc_mp, xrefc_cur_agno(cur),
			error, _RET_IP_);
	return error;
}

/* Clean up after calling xfs_refcount_finish_one. */
void
xfs_refcount_finish_one_cleanup(
	struct xfs_trans	*tp,
	struct xfs_btree_cur	*rcur,
	int			error)
{
	struct xfs_buf		*agbp;

	if (rcur == NULL)
		return;
	agbp = rcur->bc_ag.agbp;
	xfs_btree_del_cursor(rcur, error);
	if (error)
		xfs_trans_brelse(tp, agbp);
}

/*
 * Process one of the deferred refcount operations.  We pass back the
 * btree cursor to maintain our lock on the btree between calls.
 * This saves time and eliminates a buffer deadlock between the
 * superblock and the AGF because we'll always grab them in the same
 * order.
 */
int
xfs_refcount_finish_one(
	struct xfs_trans		*tp,
	enum xfs_refcount_intent_type	type,
	xfs_fsblock_t			startblock,
	xfs_filblks_t			blockcount,
	xfs_fsblock_t			*new_fsb,
	xfs_filblks_t			*new_len,
	struct xfs_btree_cur		**pcur)
{
	struct xfs_mount		*mp = tp->t_mountp;
	struct xfs_btree_cur		*rcur;
	struct xfs_buf			*agbp = NULL;
	int				error = 0;
	xfs_agnumber_t			agno;
	xfs_fsblock_t			bno;
	xfs_fsblock_t			new_bno;
	unsigned long			nr_ops = 0;
	int				shape_changes = 0;

	agno = XFS_FSB_TO_AGNO(mp, startblock);
	ASSERT(agno != NULLAGNUMBER);
	bno = XFS_FSB_TO_AGBNO(mp, startblock);

	trace_xfs_refcount_deferred(mp, XFS_FSB_TO_AGNO(mp, startblock),
			type, XFS_FSB_TO_AGBNO(mp, startblock),
			blockcount);

	if (XFS_TEST_ERROR(false, mp,
			XFS_ERRTAG_REFCOUNT_FINISH_ONE))
		return -EIO;

	/*
	 * If we haven't gotten a cursor or the cursor AG doesn't match
	 * the startblock, get one now.
	 */
	rcur = *pcur;
	if (rcur != NULL && xrefc_cur_agno(rcur) != agno) {
		nr_ops = xrefc_btree_state(rcur)->nr_ops;
		shape_changes = xrefc_btree_state(rcur)->shape_changes;
		xfs_refcount_finish_one_cleanup(tp, rcur, 0);
		rcur = NULL;
		*pcur = NULL;
	}
	if (rcur == NULL) {
		error = xfs_alloc_read_agf(tp->t_mountp, tp, agno,
				XFS_ALLOC_FLAG_FREEING, &agbp);
		if (error)
			return error;

		rcur = xfs_refcountbt_init_cursor(mp, tp, agbp, agno);
		if (!rcur) {
			error = -ENOMEM;
			goto out_cur;
		}
		xrefc_btree_state(rcur)->nr_ops = nr_ops;
		xrefc_btree_state(rcur)->shape_changes = shape_changes;
	}
	*pcur = rcur;

	switch (type) {
	case XFS_REFCOUNT_INCREASE:
		error = xfs_refcount_adjust(rcur, bno, blockcount, &new_bno,
				new_len, XFS_REFCOUNT_ADJUST_INCREASE);
		*new_fsb = XFS_AGB_TO_FSB(mp, agno, new_bno);
		break;
	case XFS_REFCOUNT_DECREASE:
		error = xfs_refcount_adjust(rcur, bno, blockcount, &new_bno,
				new_len, XFS_REFCOUNT_ADJUST_DECREASE);
		*new_fsb = XFS_AGB_TO_FSB(mp, agno, new_bno);
		break;
	case XFS_REFCOUNT_ALLOC_COW:
		*new_fsb = startblock + blockcount;
		*new_len = 0;
		error = __xfs_refcount_cow_alloc(rcur, bno, blockcount);
		break;
	case XFS_REFCOUNT_FREE_COW:
		*new_fsb = startblock + blockcount;
		*new_len = 0;
		error = __xfs_refcount_cow_free(rcur, bno, blockcount);
		break;
	default:
		ASSERT(0);
		error = -EFSCORRUPTED;
	}
	if (!error && *new_len > 0)
		trace_xfs_refcount_finish_one_leftover(mp, agno, type,
				bno, blockcount, new_bno, *new_len);
	return error;

out_cur:
	xfs_trans_brelse(tp, agbp);

	return error;
}

/*
 * Record a refcount intent for later processing.
 */
static void
__xfs_refcount_add(
	struct xfs_trans		*tp,
	enum xfs_refcount_intent_type	type,
	xfs_fsblock_t			startblock,
	xfs_filblks_t			blockcount)
{
	struct xfs_refcount_intent	*ri;

	trace_xfs_refcount_defer(tp->t_mountp,
			XFS_FSB_TO_AGNO(tp->t_mountp, startblock),
			type, XFS_FSB_TO_AGBNO(tp->t_mountp, startblock),
			blockcount);

	ri = kmem_alloc(sizeof(struct xfs_refcount_intent),
			KM_NOFS);
	INIT_LIST_HEAD(&ri->ri_list);
	ri->ri_type = type;
	ri->ri_startblock = startblock;
	ri->ri_blockcount = blockcount;

	xfs_defer_add(tp, XFS_DEFER_OPS_TYPE_REFCOUNT, &ri->ri_list);
}

/*
 * Increase the reference count of the blocks backing a file's extent.
 */
void
xfs_refcount_increase_extent(
	struct xfs_trans		*tp,
	struct xfs_bmbt_irec		*PREV)
{
	if (!xfs_sb_version_hasreflink(&tp->t_mountp->m_sb))
		return;

	__xfs_refcount_add(tp, XFS_REFCOUNT_INCREASE, PREV->br_startblock,
			PREV->br_blockcount);
}

/*
 * Decrease the reference count of the blocks backing a file's extent.
 */
void
xfs_refcount_decrease_extent(
	struct xfs_trans		*tp,
	struct xfs_bmbt_irec		*PREV)
{
	if (!xfs_sb_version_hasreflink(&tp->t_mountp->m_sb))
		return;

	__xfs_refcount_add(tp, XFS_REFCOUNT_DECREASE, PREV->br_startblock,
			PREV->br_blockcount);
}

/*
 * Given an AG extent, find the lowest-numbered run of shared blocks
 * within that range and return the range in fbno/flen.  If
 * find_end_of_shared is set, return the longest contiguous extent of
 * shared blocks; if not, just return the first extent we find.  If no
 * shared blocks are found, fbno and flen will be set to NULLFSBLOCK
 * and 0, respectively.
 */
int
xfs_refcount_find_shared(
	struct xfs_btree_cur		*cur,
	xfs_fsblock_t			bno,
	xfs_filblks_t			len,
	xfs_fsblock_t			*fbno,
	xfs_filblks_t			*flen,
	bool				find_end_of_shared)
{
	struct xfs_refcount_irec	tmp;
	int				i;
	int				have;
	int				error;

	trace_xfs_refcount_find_shared(cur->bc_mp, xrefc_cur_agno(cur),
			bno, len);

	/* By default, skip the whole range */
	*fbno = NULLFSBLOCK;
	*flen = 0;

	/* Try to find a refcount extent that crosses the start */
	error = xfs_refcount_lookup_le(cur, bno, &have);
	if (error)
		goto out_error;
	if (!have) {
		/* No left extent, look at the next one */
		error = xfs_btree_increment(cur, 0, &have);
		if (error)
			goto out_error;
		if (!have)
			goto done;
	}
	error = xfs_refcount_get_rec(cur, &tmp, &i);
	if (error)
		goto out_error;
	if (XFS_IS_CORRUPT(cur->bc_mp, i != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto out_error;
	}

	/* If the extent ends before the start, look at the next one */
	if (tmp.rc_startblock + tmp.rc_blockcount <= bno) {
		error = xfs_btree_increment(cur, 0, &have);
		if (error)
			goto out_error;
		if (!have)
			goto done;
		error = xfs_refcount_get_rec(cur, &tmp, &i);
		if (error)
			goto out_error;
		if (XFS_IS_CORRUPT(cur->bc_mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}
	}

	/* If the extent starts after the range we want, bail out */
	if (tmp.rc_startblock >= bno + len)
		goto done;

	/* We found the start of a shared extent! */
	if (tmp.rc_startblock < bno) {
		tmp.rc_blockcount -= (bno - tmp.rc_startblock);
		tmp.rc_startblock = bno;
	}

	*fbno = tmp.rc_startblock;
	*flen = min(tmp.rc_blockcount, bno + len - *fbno);
	if (!find_end_of_shared)
		goto done;

	/* Otherwise, find the end of this shared extent */
	while (*fbno + *flen < bno + len) {
		error = xfs_btree_increment(cur, 0, &have);
		if (error)
			goto out_error;
		if (!have)
			break;
		error = xfs_refcount_get_rec(cur, &tmp, &i);
		if (error)
			goto out_error;
		if (XFS_IS_CORRUPT(cur->bc_mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}
		if (tmp.rc_startblock >= bno + len ||
		    tmp.rc_startblock != *fbno + *flen)
			break;
		*flen = min(*flen + tmp.rc_blockcount, bno + len - *fbno);
	}

done:
	trace_xfs_refcount_find_shared_result(cur->bc_mp,
			xrefc_cur_agno(cur), *fbno, *flen);

out_error:
	if (error)
		trace_xfs_refcount_find_shared_error(cur->bc_mp,
				xrefc_cur_agno(cur), error, _RET_IP_);
	return error;
}

/*
 * Recovering CoW Blocks After a Crash
 *
 * Due to the way that the copy on write mechanism works, there's a window of
 * opportunity in which we can lose track of allocated blocks during a crash.
 * Because CoW uses delayed allocation in the in-core CoW fork, writeback
 * causes blocks to be allocated and stored in the CoW fork.  The blocks are
 * no longer in the free space btree but are not otherwise recorded anywhere
 * until the write completes and the blocks are mapped into the file.  A crash
 * in between allocation and remapping results in the replacement blocks being
 * lost.  This situation is exacerbated by the CoW extent size hint because
 * allocations can hang around for long time.
 *
 * However, there is a place where we can record these allocations before they
 * become mappings -- the reference count btree.  The btree does not record
 * extents with refcount == 1, so we can record allocations with a refcount of
 * 1.  Blocks being used for CoW writeout cannot be shared, so there should be
 * no conflict with shared block records.  These mappings should be created
 * when we allocate blocks to the CoW fork and deleted when they're removed
 * from the CoW fork.
 *
 * Minor nit: records for in-progress CoW allocations and records for shared
 * extents must never be merged, to preserve the property that (except for CoW
 * allocations) there are no refcount btree entries with refcount == 1.  The
 * only time this could potentially happen is when unsharing a block that's
 * adjacent to CoW allocations, so we must be careful to avoid this.
 *
 * At mount time we recover lost CoW allocations by searching the refcount
 * btree for these refcount == 1 mappings.  These represent CoW allocations
 * that were in progress at the time the filesystem went down, so we can free
 * them to get the space back.
 *
 * This mechanism is superior to creating EFIs for unmapped CoW extents for
 * several reasons -- first, EFIs pin the tail of the log and would have to be
 * periodically relogged to avoid filling up the log.  Second, CoW completions
 * will have to file an EFD and create new EFIs for whatever remains in the
 * CoW fork; this partially takes care of (1) but extent-size reservations
 * will have to periodically relog even if there's no writeout in progress.
 * This can happen if the CoW extent size hint is set, which you really want.
 * Third, EFIs cannot currently be automatically relogged into newer
 * transactions to advance the log tail.  Fourth, stuffing the log full of
 * EFIs places an upper bound on the number of CoW allocations that can be
 * held filesystem-wide at any given time.  Recording them in the refcount
 * btree doesn't require us to maintain any state in memory and doesn't pin
 * the log.
 */
/*
 * Adjust the refcounts of CoW allocations.  These allocations are "magic"
 * in that they're not referenced anywhere else in the filesystem, so we
 * stash them in the refcount btree with a refcount of 1 until either file
 * remapping (or CoW cancellation) happens.
 */
STATIC int
xfs_refcount_adjust_cow_extents(
	struct xfs_btree_cur	*cur,
	xfs_fsblock_t		bno,
	xfs_filblks_t		len,
	enum xfs_refc_adjust_op	adj)
{
	struct xfs_refcount_irec	ext, tmp;
	int				error;
	int				found_rec, found_tmp;

	if (len == 0)
		return 0;

	/* Find any overlapping refcount records */
	error = xfs_refcount_lookup_ge(cur, bno, &found_rec);
	if (error)
		goto out_error;
	error = xfs_refcount_get_rec(cur, &ext, &found_rec);
	if (error)
		goto out_error;
	if (!found_rec) {
		ext.rc_startblock = xrefc_max_startblock(cur) +
				    xrefc_cow_start(cur);
		ext.rc_blockcount = 0;
		ext.rc_refcount = 0;
	}

	switch (adj) {
	case XFS_REFCOUNT_ADJUST_COW_ALLOC:
		/* Adding a CoW reservation, there should be nothing here. */
		if (XFS_IS_CORRUPT(cur->bc_mp,
				   bno + len > ext.rc_startblock)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}

		tmp.rc_startblock = bno;
		tmp.rc_blockcount = len;
		tmp.rc_refcount = 1;
		trace_xfs_refcount_modify_extent(cur->bc_mp,
				xrefc_cur_agno(cur), &tmp);

		error = xfs_refcount_insert(cur, &tmp,
				&found_tmp);
		if (error)
			goto out_error;
		if (XFS_IS_CORRUPT(cur->bc_mp, found_tmp != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}
		break;
	case XFS_REFCOUNT_ADJUST_COW_FREE:
		/* Removing a CoW reservation, there should be one extent. */
		if (XFS_IS_CORRUPT(cur->bc_mp, ext.rc_startblock != bno)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}
		if (XFS_IS_CORRUPT(cur->bc_mp, ext.rc_blockcount != len)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}
		if (XFS_IS_CORRUPT(cur->bc_mp, ext.rc_refcount != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}

		ext.rc_refcount = 0;
		trace_xfs_refcount_modify_extent(cur->bc_mp,
				xrefc_cur_agno(cur), &ext);
		error = xfs_refcount_delete(cur, &found_rec);
		if (error)
			goto out_error;
		if (XFS_IS_CORRUPT(cur->bc_mp, found_rec != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}
		break;
	default:
		ASSERT(0);
	}

	return error;
out_error:
	trace_xfs_refcount_modify_extent_error(cur->bc_mp,
			xrefc_cur_agno(cur), error, _RET_IP_);
	return error;
}

/*
 * Add or remove refcount btree entries for CoW reservations.
 */
STATIC int
xfs_refcount_adjust_cow(
	struct xfs_btree_cur	*cur,
	xfs_fsblock_t		bno,
	xfs_filblks_t		len,
	enum xfs_refc_adjust_op	adj)
{
	bool			shape_changed;
	int			error;

	bno += xrefc_cow_start(cur);

	/*
	 * Ensure that no rcextents cross the boundary of the adjustment range.
	 */
	error = xfs_refcount_split_extent(cur, bno, &shape_changed);
	if (error)
		goto out_error;

	error = xfs_refcount_split_extent(cur, bno + len, &shape_changed);
	if (error)
		goto out_error;

	/*
	 * Try to merge with the left or right extents of the range.
	 */
	error = xfs_refcount_merge_extents(cur, &bno, &len, adj,
			XFS_FIND_RCEXT_COW, &shape_changed);
	if (error)
		goto out_error;

	/* Now that we've taken care of the ends, adjust the middle extents */
	error = xfs_refcount_adjust_cow_extents(cur, bno, len, adj);
	if (error)
		goto out_error;

	return 0;

out_error:
	trace_xfs_refcount_adjust_cow_error(cur->bc_mp, xrefc_cur_agno(cur),
			error, _RET_IP_);
	return error;
}

/*
 * Record a CoW allocation in the refcount btree.
 */
STATIC int
__xfs_refcount_cow_alloc(
	struct xfs_btree_cur	*rcur,
	xfs_fsblock_t		bno,
	xfs_filblks_t		len)
{
	trace_xfs_refcount_cow_increase(rcur->bc_mp, xrefc_cur_agno(rcur),
			bno, len);

	/* Add refcount btree reservation */
	return xfs_refcount_adjust_cow(rcur, bno, len,
			XFS_REFCOUNT_ADJUST_COW_ALLOC);
}

/*
 * Remove a CoW allocation from the refcount btree.
 */
STATIC int
__xfs_refcount_cow_free(
	struct xfs_btree_cur	*rcur,
	xfs_fsblock_t		bno,
	xfs_filblks_t		len)
{
	trace_xfs_refcount_cow_decrease(rcur->bc_mp, xrefc_cur_agno(rcur),
			bno, len);

	/* Remove refcount btree reservation */
	return xfs_refcount_adjust_cow(rcur, bno, len,
			XFS_REFCOUNT_ADJUST_COW_FREE);
}

/* Record a CoW staging extent in the refcount btree. */
void
xfs_refcount_alloc_cow_extent(
	struct xfs_trans		*tp,
	xfs_fsblock_t			fsb,
	xfs_filblks_t			len)
{
	struct xfs_mount		*mp = tp->t_mountp;

	if (!xfs_sb_version_hasreflink(&mp->m_sb))
		return;

	__xfs_refcount_add(tp, XFS_REFCOUNT_ALLOC_COW, fsb, len);

	/* Add rmap entry */
	xfs_rmap_alloc_extent(tp, fsb, len, XFS_RMAP_OWN_COW, false);
}

/* Forget a CoW staging event in the refcount btree. */
void
xfs_refcount_free_cow_extent(
	struct xfs_trans		*tp,
	xfs_fsblock_t			fsb,
	xfs_filblks_t			len)
{
	struct xfs_mount		*mp = tp->t_mountp;

	if (!xfs_sb_version_hasreflink(&mp->m_sb))
		return;

	/* Remove rmap entry */
	xfs_rmap_free_extent(tp, fsb, len, XFS_RMAP_OWN_COW, false);
	__xfs_refcount_add(tp, XFS_REFCOUNT_FREE_COW, fsb, len);
}

struct xfs_refcount_recovery {
	struct list_head		rr_list;
	struct xfs_refcount_irec	rr_rrec;
};

/* Stuff an extent on the recovery list. */
STATIC int
xfs_refcount_recover_extent(
	struct xfs_btree_cur		*cur,
	union xfs_btree_rec		*rec,
	void				*priv)
{
	struct list_head		*debris = priv;
	struct xfs_refcount_recovery	*rr;
	xfs_nlink_t			refcount;

	if (cur->bc_btnum == XFS_BTNUM_RTREFC)
		refcount = be32_to_cpu(rec->rtrefc.rc_refcount);
	else
		refcount = be32_to_cpu(rec->refc.rc_refcount);

	if (XFS_IS_CORRUPT(cur->bc_mp, refcount != 1)) {
		xfs_btree_mark_sick(cur);
		return -EFSCORRUPTED;
	}

	rr = kmem_alloc(sizeof(struct xfs_refcount_recovery), 0);
	xfs_refcount_btrec_to_irec(cur, rec, &rr->rr_rrec);
	list_add_tail(&rr->rr_list, debris);

	return 0;
}

/* Find and remove leftover CoW reservations. */
int
xfs_refcount_recover_cow_leftovers(
	struct xfs_mount		*mp,
	xfs_agnumber_t			agno)
{
	struct xfs_trans		*tp;
	struct xfs_btree_cur		*cur;
	struct xfs_buf			*agbp;
	struct xfs_refcount_recovery	*rr, *n;
	struct list_head		debris;
	union xfs_btree_irec		low;
	union xfs_btree_irec		high;
	xfs_fsblock_t			fsb;
	xfs_fsblock_t			bno;
	int				error;

	if (mp->m_sb.sb_agblocks >= XFS_REFC_COW_START)
		return -EOPNOTSUPP;

	INIT_LIST_HEAD(&debris);

	/*
	 * In this first part, we use an empty transaction to gather up
	 * all the leftover CoW extents so that we can subsequently
	 * delete them.  The empty transaction is used to avoid
	 * a buffer lock deadlock if there happens to be a loop in the
	 * refcountbt because we're allowed to re-grab a buffer that is
	 * already attached to our transaction.  When we're done
	 * recording the CoW debris we cancel the (empty) transaction
	 * and everything goes away cleanly.
	 */
	error = xfs_trans_alloc_empty(mp, &tp);
	if (error)
		return error;

	error = xfs_alloc_read_agf(mp, tp, agno, 0, &agbp);
	if (error)
		goto out_trans;
	cur = xfs_refcountbt_init_cursor(mp, tp, agbp, agno);

	/* Find all the leftover CoW staging extents. */
	memset(&low, 0, sizeof(low));
	memset(&high, 0, sizeof(high));
	low.rc.rc_startblock = XFS_REFC_COW_START;
	high.rc.rc_startblock = -1U;
	error = xfs_btree_query_range(cur, &low, &high,
			xfs_refcount_recover_extent, &debris);
	xfs_btree_del_cursor(cur, error);
	xfs_trans_brelse(tp, agbp);
	xfs_trans_cancel(tp);
	if (error)
		goto out_free;

	/* Now iterate the list to free the leftovers */
	list_for_each_entry_safe(rr, n, &debris, rr_list) {
		/* Set up transaction. */
		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, 0, 0, 0, &tp);
		if (error)
			goto out_free;

		trace_xfs_refcount_recover_extent(mp, agno, &rr->rr_rrec);

		/* Free the orphan record */
		bno = rr->rr_rrec.rc_startblock - XFS_REFC_COW_START;
		fsb = XFS_AGB_TO_FSB(mp, agno, bno);
		xfs_refcount_free_cow_extent(tp, fsb,
				rr->rr_rrec.rc_blockcount);

		/* Free the block. */
		xfs_bmap_add_free(tp, fsb, rr->rr_rrec.rc_blockcount, NULL,
				false);

		error = xfs_trans_commit(tp);
		if (error)
			goto out_free;

		list_del(&rr->rr_list);
		kmem_free(rr);
	}

	return error;
out_trans:
	xfs_trans_cancel(tp);
out_free:
	/* Free the leftover list */
	list_for_each_entry_safe(rr, n, &debris, rr_list) {
		list_del(&rr->rr_list);
		kmem_free(rr);
	}
	return error;
}

static bool
xfs_refcount_has_key_gap(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key1,
	const union xfs_btree_key	*key2)
{
	xfs_fsblock_t			next;

	next = be32_to_cpu(key1->refc.rc_startblock) + 1;
	return next != be32_to_cpu(key2->refc.rc_startblock);
}

/* Is there a record covering a given extent? */
int
xfs_refcount_has_record(
	struct xfs_btree_cur	*cur,
	xfs_fsblock_t		bno,
	xfs_filblks_t		len,
	bool			*exists)
{
	union xfs_btree_irec	low;
	union xfs_btree_irec	high;

	memset(&low, 0, sizeof(low));
	low.rc.rc_startblock = bno;
	memset(&high, 0xFF, sizeof(high));
	high.rc.rc_startblock = bno + len - 1;

	return xfs_btree_has_record(cur, &low, &high, xfs_refcount_has_key_gap,
			exists);
}
