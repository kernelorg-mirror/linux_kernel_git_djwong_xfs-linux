// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
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
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_bmap.h"
#include "xfs_icache.h"
#include "xfs_quota.h"
#include "xfs_swapext.h"
#include "xfs_trace.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans_space.h"

/* Information to help us reset reflink flag / CoW fork state after a swap. */

/* Are we swapping the data fork? */
#define XFS_SX_REFLINK_DATAFORK		(1U << 0)

/* Can we swap the flags? */
#define XFS_SX_REFLINK_SWAPFLAGS	(1U << 1)

/* Previous state of the two inodes' reflink flags. */
#define XFS_SX_REFLINK_IP1_REFLINK	(1U << 2)
#define XFS_SX_REFLINK_IP2_REFLINK	(1U << 3)


/*
 * Prepare both inodes' reflink state for an extent swap, and return our
 * findings so that xfs_swapext_reflink_finish can deal with the aftermath.
 */
unsigned int
xfs_swapext_reflink_prep(
	const struct xfs_swapext_req	*req)
{
	struct xfs_mount		*mp = req->ip1->i_mount;
	unsigned int			rs = 0;

	if (req->whichfork != XFS_DATA_FORK)
		return 0;

	/*
	 * If either file has shared blocks and we're swapping data forks, we
	 * must flag the other file as having shared blocks so that we get the
	 * shared-block rmap functions if we need to fix up the rmaps.  The
	 * flags will be switched for real by xfs_swapext_reflink_finish.
	 */
	if (xfs_is_reflink_inode(req->ip1))
		rs |= XFS_SX_REFLINK_IP1_REFLINK;
	if (xfs_is_reflink_inode(req->ip2))
		rs |= XFS_SX_REFLINK_IP2_REFLINK;

	if (rs & XFS_SX_REFLINK_IP1_REFLINK)
		req->ip2->i_d.di_flags2 |= XFS_DIFLAG2_REFLINK;
	if (rs & XFS_SX_REFLINK_IP2_REFLINK)
		req->ip1->i_d.di_flags2 |= XFS_DIFLAG2_REFLINK;

	/*
	 * If either file had the reflink flag set before; and the two files'
	 * reflink state was different; and we're swapping the entirety of both
	 * files, then we can exchange the reflink flags at the end.
	 * Otherwise, we propagate the reflink flag from either file to the
	 * other file.
	 *
	 * Note that we've only set the _REFLINK flags of the reflink state, so
	 * we can cheat and use hweight32 for the reflink flag test.
	 *
	 */
	if (hweight32(rs) == 1 && req->startoff1 == 0 && req->startoff2 == 0 &&
	    req->blockcount == XFS_B_TO_FSB(mp, req->ip1->i_d.di_size) &&
	    req->blockcount == XFS_B_TO_FSB(mp, req->ip2->i_d.di_size))
		rs |= XFS_SX_REFLINK_SWAPFLAGS;

	rs |= XFS_SX_REFLINK_DATAFORK;
	return rs;
}

/*
 * If the reflink flag is set on either inode, make sure it has an incore CoW
 * fork, since all reflink inodes must have them.  If there's a CoW fork and it
 * has extents in it, make sure the inodes are tagged appropriately so that
 * speculative preallocations can be GC'd if we run low of space.
 */
static inline void
xfs_swapext_ensure_cowfork(
	struct xfs_inode	*ip)
{
	struct xfs_ifork	*cfork;

	if (xfs_is_reflink_inode(ip))
		xfs_ifork_init_cow(ip);

	cfork = XFS_IFORK_PTR(ip, XFS_COW_FORK);
	if (!cfork)
		return;
	if (cfork->if_bytes > 0)
		xfs_inode_set_cowblocks_tag(ip);
	else
		xfs_inode_clear_cowblocks_tag(ip);
}

/*
 * Set both inodes' ondisk reflink flags to their final state and ensure that
 * the incore state is ready to go.
 */
void
xfs_swapext_reflink_finish(
	struct xfs_trans		*tp,
	const struct xfs_swapext_req	*req,
	unsigned int			rs)
{
	if (!(rs & XFS_SX_REFLINK_DATAFORK))
		return;

	if (rs & XFS_SX_REFLINK_SWAPFLAGS) {
		/* Exchange the reflink inode flags and log them. */
		req->ip1->i_d.di_flags2 &= ~XFS_DIFLAG2_REFLINK;
		if (rs & XFS_SX_REFLINK_IP2_REFLINK)
			req->ip1->i_d.di_flags2 |= XFS_DIFLAG2_REFLINK;

		req->ip2->i_d.di_flags2 &= ~XFS_DIFLAG2_REFLINK;
		if (rs & XFS_SX_REFLINK_IP1_REFLINK)
			req->ip2->i_d.di_flags2 |= XFS_DIFLAG2_REFLINK;

		xfs_trans_log_inode(tp, req->ip1, XFS_ILOG_CORE);
		xfs_trans_log_inode(tp, req->ip2, XFS_ILOG_CORE);
	}

	xfs_swapext_ensure_cowfork(req->ip1);
	xfs_swapext_ensure_cowfork(req->ip2);
}

/* Schedule an atomic extent swap. */
static inline void
xfs_swapext_schedule(
	struct xfs_trans		*tp,
	struct xfs_swapext_intent	*sxi)
{
	trace_xfs_swapext_defer(tp->t_mountp, sxi);
	xfs_defer_add(tp, XFS_DEFER_OPS_TYPE_SWAPEXT, &sxi->sxi_list);
}

/* Reschedule an atomic extent swap on behalf of log recovery. */
void
xfs_swapext_reschedule(
	struct xfs_trans		*tp,
	const struct xfs_swapext_intent	*sxi)
{
	struct xfs_swapext_intent	*new_sxi;

	new_sxi = kmem_alloc(sizeof(struct xfs_swapext_intent), KM_NOFS);
	memcpy(new_sxi, sxi, sizeof(*new_sxi));
	INIT_LIST_HEAD(&new_sxi->sxi_list);

	xfs_swapext_schedule(tp, new_sxi);
}

/*
 * Adjust the on-disk inode size upwards if needed so that we never map extents
 * into the file past EOF.  This is crucial so that log recovery won't get
 * confused by the sudden appearance of post-eof extents.
 */
STATIC void
xfs_swapext_update_size(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	struct xfs_bmbt_irec	*imap,
	xfs_fsize_t		new_isize)
{
	struct xfs_mount	*mp = tp->t_mountp;
	xfs_fsize_t		len;

	if (new_isize < 0)
		return;

	len = min(XFS_FSB_TO_B(mp, imap->br_startoff + imap->br_blockcount),
		  new_isize);

	if (len <= ip->i_d.di_size)
		return;

	trace_xfs_swapext_update_inode_size(ip, len);

	ip->i_d.di_size = len;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
}

/* Do we have more work to do to finish this operation? */
bool
xfs_swapext_has_more_work(
	struct xfs_swapext_intent	*sxi)
{
	return sxi->sxi_blockcount > 0;
}

/* Check all extents to make sure we can actually swap them. */
int
xfs_swapext_check_extents(
	struct xfs_mount		*mp,
	const struct xfs_swapext_req	*req)
{
	struct xfs_ifork		*ifp1, *ifp2;

	/* No fork? */
	ifp1 = XFS_IFORK_PTR(req->ip1, req->whichfork);
	ifp2 = XFS_IFORK_PTR(req->ip2, req->whichfork);
	if (!ifp1 || !ifp2)
		return -EINVAL;

	/* We don't know how to swap local format forks. */
	if (ifp1->if_format == XFS_DINODE_FMT_LOCAL ||
	    ifp2->if_format == XFS_DINODE_FMT_LOCAL)
		return -EINVAL;

	/* We don't support realtime files yet. */
	return XFS_IS_REALTIME_INODE(req->ip1) ? -EINVAL : 0;
}

/* Finish one extent swap, possibly log more. */
int
xfs_swapext_finish_one(
	struct xfs_trans		*tp,
	struct xfs_swapext_intent	*sxi)
{
	struct xfs_bmbt_irec		irec1, irec2;
	int				whichfork;
	int				nimaps;
	int				bmap_flags;
	int				error;

	whichfork = (sxi->sxi_flags & XFS_SWAP_EXTENT_ATTR_FORK) ?
			XFS_ATTR_FORK : XFS_DATA_FORK;
	bmap_flags = xfs_bmapi_aflag(whichfork);

	while (sxi->sxi_blockcount > 0) {
		int64_t		ip1_delta = 0, ip2_delta = 0;

		/* Read extent from the first file */
		nimaps = 1;
		error = xfs_bmapi_read(sxi->sxi_ip1, sxi->sxi_startoff1,
				sxi->sxi_blockcount, &irec1, &nimaps,
				bmap_flags);
		if (error)
			return error;
		if (nimaps != 1 ||
		    irec1.br_startblock == DELAYSTARTBLOCK ||
		    irec1.br_startoff != sxi->sxi_startoff1) {
			/*
			 * We should never get no mapping or a delalloc extent
			 * or something that doesn't match what we asked for,
			 * since the caller flushed both inodes and we hold the
			 * ILOCKs for both inodes.
			 */
			ASSERT(0);
			return -EINVAL;
		}

		/*
		 * If the caller told us to ignore sparse areas of file1, jump
		 * ahead to the next region.
		 */
		if ((sxi->sxi_flags & XFS_SWAP_EXTENT_SKIP_FILE1_HOLES) &&
		    irec1.br_startblock == HOLESTARTBLOCK) {
			trace_xfs_swapext_extent1(sxi->sxi_ip1, &irec1);

			sxi->sxi_startoff1 += irec1.br_blockcount;
			sxi->sxi_startoff2 += irec1.br_blockcount;
			sxi->sxi_blockcount -= irec1.br_blockcount;
			continue;
		}

		/* Read extent from the second file */
		nimaps = 1;
		error = xfs_bmapi_read(sxi->sxi_ip2, sxi->sxi_startoff2,
				irec1.br_blockcount, &irec2, &nimaps,
				bmap_flags);
		if (error)
			return error;
		if (nimaps != 1 ||
		    irec2.br_startblock == DELAYSTARTBLOCK ||
		    irec2.br_startoff != sxi->sxi_startoff2) {
			/*
			 * We should never get no mapping or a delalloc extent
			 * or something that doesn't match what we asked for,
			 * since the caller flushed both inodes and we hold the
			 * ILOCKs for both inodes.
			 */
			ASSERT(0);
			return -EINVAL;
		}

		/*
		 * We can only swap as many blocks as the smaller of the two
		 * extent maps.
		 */
		irec1.br_blockcount = min(irec1.br_blockcount,
					  irec2.br_blockcount);

		trace_xfs_swapext_extent1(sxi->sxi_ip1, &irec1);
		trace_xfs_swapext_extent2(sxi->sxi_ip2, &irec2);

		/*
		 * Two extents mapped to the same physical block must not have
		 * different states; that's filesystem corruption.  Move on to
		 * the next extent if they're both holes or both the same
		 * physical extent.
		 */
		if (irec1.br_startblock == irec2.br_startblock) {
			if (irec1.br_state != irec2.br_state)
				return -EFSCORRUPTED;

			sxi->sxi_startoff1 += irec1.br_blockcount;
			sxi->sxi_startoff2 += irec1.br_blockcount;
			sxi->sxi_blockcount -= irec1.br_blockcount;
			continue;
		}

		/* Update quota accounting. */
		if (xfs_bmap_is_real_extent(&irec1)) {
			ip1_delta -= irec1.br_blockcount;
			ip2_delta += irec1.br_blockcount;
		}
		if (xfs_bmap_is_real_extent(&irec2)) {
			ip1_delta += irec2.br_blockcount;
			ip2_delta -= irec2.br_blockcount;
		}

		if (ip1_delta) {
			xfs_trans_mod_dquot_byino(tp, sxi->sxi_ip1,
					XFS_TRANS_DQ_BCOUNT, ip1_delta);
		}
		if (ip2_delta) {
			xfs_trans_mod_dquot_byino(tp, sxi->sxi_ip2,
					XFS_TRANS_DQ_BCOUNT, ip2_delta);
		}

		/* Remove both mappings. */
		xfs_bmap_unmap_extent(tp, sxi->sxi_ip1, whichfork, &irec1);
		xfs_bmap_unmap_extent(tp, sxi->sxi_ip2, whichfork, &irec2);

		/*
		 * Re-add both mappings.  We swap the file offsets between the
		 * two maps and add the opposite map, which has the effect of
		 * filling the logical offsets we just unmapped, but with with
		 * the physical mapping information swapped.
		 */
		swap(irec1.br_startoff, irec2.br_startoff);
		xfs_bmap_map_extent(tp, sxi->sxi_ip1, whichfork, &irec2);
		xfs_bmap_map_extent(tp, sxi->sxi_ip2, whichfork, &irec1);

		/* Make sure we're not mapping extents past EOF. */
		if (whichfork == XFS_DATA_FORK) {
			xfs_swapext_update_size(tp, sxi->sxi_ip1, &irec2,
					sxi->sxi_isize1);
			xfs_swapext_update_size(tp, sxi->sxi_ip2, &irec1,
					sxi->sxi_isize2);
		}

		/*
		 * Advance our cursor and exit.   The caller (either defer ops
		 * or log recovery) will log the SXD item, and if *blockcount
		 * is nonzero, it will log a new SXI item for the remainder
		 * and call us back.
		 */
		sxi->sxi_startoff1 += irec1.br_blockcount;
		sxi->sxi_startoff2 += irec1.br_blockcount;
		sxi->sxi_blockcount -= irec1.br_blockcount;
		break;
	}

	/*
	 * If the caller asked us to exchange the file sizes and we're done
	 * moving extents, update the ondisk file sizes now.
	 */
	if (sxi->sxi_blockcount == 0 &&
	    (sxi->sxi_flags & XFS_SWAP_EXTENT_SET_SIZES)) {
		sxi->sxi_ip1->i_d.di_size = sxi->sxi_isize1;
		sxi->sxi_ip2->i_d.di_size = sxi->sxi_isize2;

		xfs_trans_log_inode(tp, sxi->sxi_ip1, XFS_ILOG_CORE);
		xfs_trans_log_inode(tp, sxi->sxi_ip2, XFS_ILOG_CORE);
	}

	if (xfs_swapext_has_more_work(sxi))
		trace_xfs_swapext_defer(tp->t_mountp, sxi);

	return 0;
}

/* Estimate the bmbt and rmapbt overhead required to exchange extents. */
static int
xfs_swapext_estimate_overhead(
	const struct xfs_swapext_req	*req,
	struct xfs_swapext_res		*res)
{
	struct xfs_mount		*mp = req->ip1->i_mount;
	unsigned int			bmbt_overhead;

	/*
	 * Compute the amount of bmbt blocks we should reserve for each file.
	 *
	 * Conceptually this shouldn't affect the shape of either bmbt, but
	 * since we atomically move extents one by one, we reserve enough space
	 * to handle a bmbt split for each remap operation (t1).
	 *
	 * However, we must be careful to handle a corner case where the
	 * repeated unmap and map activities could result in ping-ponging of
	 * the btree shape.  This behavior can come from one of two sources:
	 *
	 * An inode's extent list could have just enough records to straddle
	 * the btree format boundary. If so, the inode could bounce between
	 * btree <-> extent format on unmap -> remap cycles, freeing and
	 * allocating a bmapbt block each time.
	 *
	 * The same thing can happen if we have just enough records in a block
	 * to bounce between one and two leaf blocks. If there aren't enough
	 * sibling blocks to absorb or donate some records, we end up reshaping
	 * the tree with every remap operation.  This doesn't seem to happen if
	 * we have more than four bmbt leaf blocks, so we'll make that the
	 * lower bound on the pingponging (t2).
	 *
	 * Therefore, we use XFS_TRANS_RES_FDBLKS so that freed bmbt blocks
	 * are accounted back to the transaction block reservation.
	 */
	bmbt_overhead = XFS_NEXTENTADD_SPACE_RES(mp, res->nr_exchanges,
						 req->whichfork);
	res->ip1_bcount += bmbt_overhead;
	res->ip2_bcount += bmbt_overhead;
	res->resblks += 2 * bmbt_overhead;

	/* Apply similar logic to rmapbt reservations. */
	if (xfs_sb_version_hasrmapbt(&mp->m_sb)) {
		unsigned int	rmapbt_overhead;

		if (!XFS_IS_REALTIME_INODE(req->ip1))
			rmapbt_overhead = XFS_NRMAPADD_SPACE_RES(mp,
							res->nr_exchanges);
		else
			rmapbt_overhead = 0;
		res->resblks += 2 * rmapbt_overhead;
	}

	trace_xfs_swapext_estimate(req, res);

	if (res->resblks > UINT_MAX)
		return -ENOSPC;
	return 0;
}

/*
 * Estimate the number of exchange operations and the number of file blocks
 * in each file that will be affected by the exchange operation.
 */
int
xfs_swapext_estimate(
	const struct xfs_swapext_req	*req,
	struct xfs_swapext_res		*res)
{
	struct xfs_swapext_req		r = *req; /* struct copy */
	struct xfs_bmbt_irec		irec1, irec2;
	xfs_filblks_t			ip1_blocks = 0, ip2_blocks = 0;
	int				bmap_flags;
	int				nimaps;
	int				error;

	bmap_flags = xfs_bmapi_aflag(req->whichfork);
	memset(res, 0, sizeof(struct xfs_swapext_res));

	while (r.blockcount > 0) {
		/* Read extent from the first file */
		nimaps = 1;
		error = xfs_bmapi_read(r.ip1, r.startoff1, r.blockcount,
				&irec1, &nimaps, bmap_flags);
		if (error)
			return error;
		if (irec1.br_startblock == DELAYSTARTBLOCK ||
		    irec1.br_startoff != r.startoff1) {
			/*
			 * We should never get no mapping or a delalloc extent
			 * or something that doesn't match what we asked for,
			 * since the caller flushed both inodes and we hold the
			 * ILOCKs for both inodes.
			 */
			ASSERT(0);
			return -EINVAL;
		}

		/*
		 * If the caller told us to ignore sparse areas of file1, jump
		 * ahead to the next region.
		 */
		if ((req->flags & XFS_SWAPEXT_SKIP_FILE1_HOLES) &&
		    irec1.br_startblock == HOLESTARTBLOCK) {
			r.startoff1 += irec1.br_blockcount;
			r.startoff2 += irec1.br_blockcount;
			r.blockcount -= irec1.br_blockcount;
			continue;
		}

		/* Read extent from the second file */
		nimaps = 1;
		error = xfs_bmapi_read(r.ip2, r.startoff2,
				irec1.br_blockcount, &irec2, &nimaps,
				bmap_flags);
		if (error)
			return error;
		if (irec2.br_startblock == DELAYSTARTBLOCK ||
		    irec2.br_startoff != r.startoff2) {
			/*
			 * We should never get no mapping or a delalloc extent
			 * or something that doesn't match what we asked for,
			 * since the caller flushed both inodes and we hold the
			 * ILOCKs for both inodes.
			 */
			ASSERT(0);
			return -EINVAL;
		}

		/*
		 * We can only swap as many blocks as the smaller of the two
		 * extent maps.
		 */
		irec1.br_blockcount = min(irec1.br_blockcount,
					  irec2.br_blockcount);

		/*
		 * Two extents mapped to the same physical block must not have
		 * different states; that's filesystem corruption.  Move on to
		 * the next extent if they're both holes or both the same
		 * physical extent.
		 */
		if (irec1.br_startblock == irec2.br_startblock) {
			if (irec1.br_state != irec2.br_state)
				return -EFSCORRUPTED;

			r.startoff1 += irec1.br_blockcount;
			r.startoff2 += irec1.br_blockcount;
			r.blockcount -= irec1.br_blockcount;
			continue;
		}

		/* Update accounting. */
		if (xfs_bmap_is_real_extent(&irec1))
			ip1_blocks += irec1.br_blockcount;
		if (xfs_bmap_is_real_extent(&irec2))
			ip2_blocks += irec2.br_blockcount;
		res->nr_exchanges++;

		/* Advance our cursor and move on. */
		r.startoff1 += irec1.br_blockcount;
		r.startoff2 += irec1.br_blockcount;
		r.blockcount -= irec1.br_blockcount;
	}

	/* Account for the blocks that are being exchanged. */
	if (XFS_IS_REALTIME_INODE(req->ip1) &&
	    req->whichfork == XFS_DATA_FORK) {
		res->ip1_rtbcount = ip1_blocks;
		res->ip2_rtbcount = ip2_blocks;
	} else {
		res->ip1_bcount = ip1_blocks;
		res->ip2_bcount = ip2_blocks;
	}

	return xfs_swapext_estimate_overhead(req, res);
}

static void
xfs_swapext_init_intent(
	struct xfs_swapext_intent	*sxi,
	const struct xfs_swapext_req	*req)
{
	INIT_LIST_HEAD(&sxi->sxi_list);
	sxi->sxi_flags = 0;
	if (req->whichfork == XFS_ATTR_FORK)
		sxi->sxi_flags |= XFS_SWAP_EXTENT_ATTR_FORK;
	sxi->sxi_isize1 = sxi->sxi_isize2 = -1;
	if (req->whichfork == XFS_DATA_FORK &&
	    (req->flags & XFS_SWAPEXT_SET_SIZES)) {
		sxi->sxi_flags |= XFS_SWAP_EXTENT_SET_SIZES;
		sxi->sxi_isize1 = req->ip2->i_d.di_size;
		sxi->sxi_isize2 = req->ip1->i_d.di_size;
	}
	if (req->flags & XFS_SWAPEXT_SKIP_FILE1_HOLES)
		sxi->sxi_flags |= XFS_SWAP_EXTENT_SKIP_FILE1_HOLES;
	sxi->sxi_ip1 = req->ip1;
	sxi->sxi_ip2 = req->ip2;
	sxi->sxi_startoff1 = req->startoff1;
	sxi->sxi_startoff2 = req->startoff2;
	sxi->sxi_blockcount = req->blockcount;
}

/*
 * Swap a range of extents from one inode to another.  If the atomic swap
 * feature is enabled, then the operation progress can be resumed even if the
 * system goes down.
 *
 * The caller must ensure the inodes must be joined to the transaction and
 * ILOCKd; they will still be joined to the transaction at exit.
 */
int
xfs_swapext(
	struct xfs_trans		**tpp,
	const struct xfs_swapext_req	*req)
{
	struct xfs_swapext_intent	*sxi;
	unsigned int			reflink_state;
	int				error;

	ASSERT(xfs_isilocked(req->ip1, XFS_ILOCK_EXCL));
	ASSERT(xfs_isilocked(req->ip2, XFS_ILOCK_EXCL));
	ASSERT(req->whichfork != XFS_COW_FORK);
	if (req->flags & XFS_SWAPEXT_SET_SIZES)
		ASSERT(req->whichfork == XFS_DATA_FORK);

	reflink_state = xfs_swapext_reflink_prep(req);

	sxi = kmem_alloc(sizeof(struct xfs_swapext_intent), KM_NOFS);
	xfs_swapext_init_intent(sxi, req);
	xfs_swapext_schedule(*tpp, sxi);

	error = xfs_defer_finish(tpp);
	if (error)
		return error;

	xfs_swapext_reflink_finish(*tpp, req, reflink_state);
	return 0;
}
