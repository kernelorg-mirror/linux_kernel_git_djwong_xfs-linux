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
	struct xfs_inode	*ip1,
	struct xfs_inode	*ip2,
	int			whichfork,
	xfs_fileoff_t		startoff1,
	xfs_fileoff_t		startoff2,
	xfs_filblks_t		blockcount)
{
	struct xfs_mount	*mp = ip1->i_mount;
	unsigned int		rs = 0;

	if (whichfork != XFS_DATA_FORK)
		return 0;

	/*
	 * If either file has shared blocks and we're swapping data forks, we
	 * must flag the other file as having shared blocks so that we get the
	 * shared-block rmap functions if we need to fix up the rmaps.  The
	 * flags will be switched for real by xfs_swapext_reflink_finish.
	 */
	if (xfs_is_reflink_inode(ip1))
		rs |= XFS_SX_REFLINK_IP1_REFLINK;
	if (xfs_is_reflink_inode(ip2))
		rs |= XFS_SX_REFLINK_IP2_REFLINK;

	if (rs & XFS_SX_REFLINK_IP1_REFLINK)
		ip2->i_d.di_flags2 |= XFS_DIFLAG2_REFLINK;
	if (rs & XFS_SX_REFLINK_IP2_REFLINK)
		ip1->i_d.di_flags2 |= XFS_DIFLAG2_REFLINK;

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
	if (hweight32(rs) == 1 && startoff1 == 0 && startoff2 == 0 &&
	    blockcount == XFS_B_TO_FSB(mp, ip1->i_d.di_size) &&
	    blockcount == XFS_B_TO_FSB(mp, ip2->i_d.di_size))
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
	struct xfs_trans	*tp,
	struct xfs_inode	*ip1,
	struct xfs_inode	*ip2,
	unsigned int		rs)
{
	if (!(rs & XFS_SX_REFLINK_DATAFORK))
		return;

	if (rs & XFS_SX_REFLINK_SWAPFLAGS) {
		/* Exchange the reflink inode flags and log them. */
		ip1->i_d.di_flags2 &= ~XFS_DIFLAG2_REFLINK;
		if (rs & XFS_SX_REFLINK_IP2_REFLINK)
			ip1->i_d.di_flags2 |= XFS_DIFLAG2_REFLINK;

		ip2->i_d.di_flags2 &= ~XFS_DIFLAG2_REFLINK;
		if (rs & XFS_SX_REFLINK_IP1_REFLINK)
			ip2->i_d.di_flags2 |= XFS_DIFLAG2_REFLINK;

		xfs_trans_log_inode(tp, ip1, XFS_ILOG_CORE);
		xfs_trans_log_inode(tp, ip2, XFS_ILOG_CORE);
	}

	xfs_swapext_ensure_cowfork(ip1);
	xfs_swapext_ensure_cowfork(ip2);
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
		if (xfs_bmap_is_mapped_extent(&irec1)) {
			ip1_delta -= irec1.br_blockcount;
			ip2_delta += irec1.br_blockcount;
		}
		if (xfs_bmap_is_mapped_extent(&irec2)) {
			ip1_delta += irec2.br_blockcount;
			ip2_delta -= irec2.br_blockcount;
		}

		if (ip1_delta)
			xfs_trans_mod_dquot_byino(tp, sxi->sxi_ip1,
					XFS_TRANS_DQ_BCOUNT, ip1_delta);
		if (ip2_delta)
			xfs_trans_mod_dquot_byino(tp, sxi->sxi_ip2,
					XFS_TRANS_DQ_BCOUNT, ip2_delta);

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
	 * If we've reached the end of the remap operation and the caller
	 * wanted us to exchange the sizes, do that now.
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

static void
xfs_swapext_init_intent(
	struct xfs_swapext_intent	*sxi,
	struct xfs_inode		*ip1,
	struct xfs_inode		*ip2,
	int				whichfork,
	xfs_fileoff_t			startoff1,
	xfs_fileoff_t			startoff2,
	xfs_filblks_t			blockcount,
	unsigned int			flags)
{
	INIT_LIST_HEAD(&sxi->sxi_list);
	sxi->sxi_flags = 0;
	if (whichfork == XFS_ATTR_FORK)
		sxi->sxi_flags |= XFS_SWAP_EXTENT_ATTR_FORK;
	sxi->sxi_isize1 = sxi->sxi_isize2 = -1;
	if (whichfork == XFS_DATA_FORK && (flags & XFS_SWAPEXT_SET_SIZES)) {
		sxi->sxi_flags |= XFS_SWAP_EXTENT_SET_SIZES;
		sxi->sxi_isize1 = ip2->i_d.di_size;
		sxi->sxi_isize2 = ip1->i_d.di_size;
	}
	sxi->sxi_ip1 = ip1;
	sxi->sxi_ip2 = ip2;
	sxi->sxi_startoff1 = startoff1;
	sxi->sxi_startoff2 = startoff2;
	sxi->sxi_blockcount = blockcount;
}

/*
 * Atomically swap a range of extents from one inode to another.
 *
 * The caller must ensure the inodes must be joined to the transaction and
 * ILOCKd; they will still be joined to the transaction at exit.
 */
int
xfs_swapext_atomic(
	struct xfs_trans		**tpp,
	struct xfs_inode		*ip1,
	struct xfs_inode		*ip2,
	int				whichfork,
	xfs_fileoff_t			startoff1,
	xfs_fileoff_t			startoff2,
	xfs_filblks_t			blockcount,
	unsigned int			flags)
{
	struct xfs_swapext_intent	*sxi;
	unsigned int			state;
	int				error;

	ASSERT(xfs_isilocked(ip1, XFS_ILOCK_EXCL));
	ASSERT(xfs_isilocked(ip2, XFS_ILOCK_EXCL));
	ASSERT(whichfork != XFS_COW_FORK);
	ASSERT(whichfork == XFS_DATA_FORK || !(flags & XFS_SWAPEXT_SET_SIZES));

	state = xfs_swapext_reflink_prep(ip1, ip2, whichfork, startoff1,
			startoff2, blockcount);

	sxi = kmem_alloc(sizeof(struct xfs_swapext_intent), KM_NOFS);
	xfs_swapext_init_intent(sxi, ip1, ip2, whichfork, startoff1, startoff2,
			blockcount, flags);
	xfs_swapext_schedule(*tpp, sxi);

	error = xfs_defer_finish(tpp);
	if (error)
		return error;

	xfs_swapext_reflink_finish(*tpp, ip1, ip2, state);
	return 0;
}
