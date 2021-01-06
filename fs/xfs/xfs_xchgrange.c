// SPDX-License-Identifier: GPL-2.0-or-later
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
#include "xfs_defer.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_quota.h"
#include "xfs_bmap_util.h"
#include "xfs_reflink.h"
#include "xfs_trace.h"
#include "xfs_swapext.h"
#include "xfs_xchgrange.h"
#include "xfs_sb.h"

/* Lock (and optionally join) two inodes for a file range exchange. */
void
xfs_xchg_range_ilock(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip1,
	struct xfs_inode	*ip2)
{
	if (ip1 != ip2)
		xfs_lock_two_inodes(ip1, XFS_ILOCK_EXCL,
				    ip2, XFS_ILOCK_EXCL);
	else
		xfs_ilock(ip1, XFS_ILOCK_EXCL);
	if (tp) {
		xfs_trans_ijoin(tp, ip1, 0);
		if (ip2 != ip1)
			xfs_trans_ijoin(tp, ip2, 0);
	}

}

/* Unlock two inodes after a file range exchange operation. */
void
xfs_xchg_range_iunlock(
	struct xfs_inode	*ip1,
	struct xfs_inode	*ip2)
{
	if (ip2 != ip1)
		xfs_iunlock(ip2, XFS_ILOCK_EXCL);
	xfs_iunlock(ip1, XFS_ILOCK_EXCL);
}

/*
 * Estimate the resource requirements to exchange file contents between the two
 * files.  The caller is required to hold the IOLOCK and the MMAPLOCK and to
 * have flushed both inodes' pagecache and active direct-ios.
 */
int
xfs_xchg_range_estimate(
	const struct xfs_swapext_req	*req,
	struct xfs_swapext_res		*res)
{
	int				error;

	xfs_xchg_range_ilock(NULL, req->ip1, req->ip2);
	error = xfs_swapext_estimate(req, res);
	xfs_xchg_range_iunlock(req->ip1, req->ip2);
	return error;
}

/* Prepare two files to have their data exchanged. */
int
xfs_xchg_range_prep(
	struct file		*file1,
	struct file		*file2,
	struct file_xchg_range	*fxr)
{
	struct xfs_inode	*ip1 = XFS_I(file_inode(file1));
	struct xfs_inode	*ip2 = XFS_I(file_inode(file2));
	int			ret;

	/* Verify both files are either real-time or non-realtime */
	if (XFS_IS_REALTIME_INODE(ip1) != XFS_IS_REALTIME_INODE(ip2))
		return -EINVAL;

	/*
	 * The alignment checks in the VFS helpers cannot deal with allocation
	 * units that are not powers of 2.  This can happen with the realtime
	 * volume if the extent size is set.  Note that alignment checks are
	 * skipped if FULL_FILES is set.
	 */
	if (!(fxr->flags & FILE_XCHG_RANGE_FULL_FILES) &&
	    !is_power_of_2(xfs_inode_alloc_unitsize(ip2)))
		return -EOPNOTSUPP;

	ret = generic_xchg_file_range_prep(file1, file2, fxr,
			xfs_inode_alloc_unitsize(ip2));
	if (ret)
		return ret;

	/* Attach dquots to both inodes before changing block maps. */
	ret = xfs_qm_dqattach(ip2);
	if (ret)
		return ret;
	ret = xfs_qm_dqattach(ip1);
	if (ret)
		return ret;

	/* Flush the relevant ranges of both files. */
	ret = xfs_flush_unmap_range(ip2, fxr->file2_offset, fxr->length);
	if (ret)
		return ret;
	return xfs_flush_unmap_range(ip1, fxr->file1_offset, fxr->length);
}

struct xchg_quota_retries {
	bool	ip1_resblks;
	bool	ip2_resblks;
	bool	ip1_rtblks;
	bool	ip2_rtblks;
};

static inline bool
xchg_quota_want_retry(const struct xchg_quota_retries *qretry)
{
	return qretry->ip1_resblks || qretry->ip2_resblks ||
		qretry->ip1_rtblks || qretry->ip2_rtblks;
}

/*
 * Obtain a quota reservation to make sure we don't hit EDQUOT.  We can skip
 * this if quota enforcement is disabled or if both inodes' dquots are the
 * same.  The qretry structure must be initialized to zeroes before the first
 * call to this function.
 */
STATIC int
xfs_xchg_range_reserve_quota(
	struct xfs_trans		**tpp,
	const struct xfs_swapext_req	*req,
	const struct xfs_swapext_res	*res,
	struct xchg_quota_retries	*qretry)
{
	int64_t				delta;
	unsigned int			qflags;
	int				error;

	/*
	 * Don't bother with a quota reservation if we're not enforcing them
	 * or the two inodes have the same dquots.
	 */
	if (!XFS_IS_QUOTA_ON((*tpp)->t_mountp) || req->ip1 == req->ip2 ||
	    (req->ip1->i_udquot == req->ip2->i_udquot &&
	     req->ip1->i_gdquot == req->ip2->i_gdquot &&
	     req->ip1->i_pdquot == req->ip2->i_pdquot))
		return 0;

	/*
	 * If we find that any of the four quota reservations have landed us
	 * back here to retry a quota allocation after clearing space, we must
	 * treat all the previous reservations as being in retry mode (i.e.
	 * instant death on EDQUOT/ENOSPC) to avoid infinite retry loops.
	 */
	if (qretry->ip2_rtblks) {
		qretry->ip2_resblks = qretry->ip1_resblks = true;
		qretry->ip1_rtblks = true;
	} else if (qretry->ip1_rtblks)
		qretry->ip2_resblks = qretry->ip1_resblks = true;
	else if (qretry->ip2_resblks)
		qretry->ip1_resblks = true;

	/*
	 * For each file, compute the net gain in the number of regular blocks
	 * that will be mapped into that file and reserve that much quota.  The
	 * quota counts must be able to absorb at least that much space.
	 */
	qflags = XFS_QMOPT_RES_REGBLKS;
	delta = res->ip2_bcount - res->ip1_bcount;
	if (delta > 0) {
		error = xfs_trans_reserve_quota_nblks(tpp, req->ip1, delta, 0,
				qflags, &qretry->ip1_resblks);
		if (!(*tpp))
			xfs_iunlock(req->ip2, XFS_ILOCK_EXCL);
		if (error || qretry->ip1_resblks)
			return error;
	} else if (delta < 0) {
		error = xfs_trans_reserve_quota_nblks(tpp, req->ip2, -delta, 0,
				qflags, &qretry->ip2_resblks);
		if (!(*tpp))
			xfs_iunlock(req->ip1, XFS_ILOCK_EXCL);
		if (error || qretry->ip2_resblks)
			return error;
	}

	/*
	 * For each file, compute the net gain in the number of rt blocks that
	 * will be mapped into that file and reserve that much quota.  The
	 * quota counts must be able to absorb at least that much space.
	 */
	qflags = XFS_QMOPT_RES_RTBLKS;
	delta = res->ip2_rtbcount - res->ip1_rtbcount;
	if (delta > 0) {
		error = xfs_trans_reserve_quota_nblks(tpp, req->ip1, delta, 0,
				qflags, &qretry->ip1_rtblks);
		if (!(*tpp))
			xfs_iunlock(req->ip2, XFS_ILOCK_EXCL);
		if (error || qretry->ip2_rtblks)
			return error;
	} else if (delta < 0) {
		error = xfs_trans_reserve_quota_nblks(tpp, req->ip2, -delta, 0,
				qflags, &qretry->ip2_rtblks);
		if (!(*tpp))
			xfs_iunlock(req->ip1, XFS_ILOCK_EXCL);
		if (error || qretry->ip2_rtblks)
			return error;
	}

	/*
	 * For each file, forcibly reserve the gross gain in mapped blocks so
	 * that we don't trip over any quota block reservation assertions.
	 * We must reserve the gross gain because the quota code subtracts from
	 * bcount the number of blocks that we unmap; it does not add that
	 * quantity back to the quota block reservation.
	 */
	qflags = XFS_QMOPT_FORCE_RES | XFS_QMOPT_RES_REGBLKS;
	error = xfs_trans_reserve_quota_nblks(tpp, req->ip1, res->ip1_bcount,
			0, qflags, NULL);
	if (error)
		return error;

	error = xfs_trans_reserve_quota_nblks(tpp, req->ip2, res->ip2_bcount,
			0, qflags, NULL);
	if (error)
		return error;

	qflags = XFS_QMOPT_FORCE_RES | XFS_QMOPT_RES_RTBLKS;
	error = xfs_trans_reserve_quota_nblks(tpp, req->ip1, res->ip1_rtbcount,
			0, qflags, NULL);
	if (error)
		return error;

	error = xfs_trans_reserve_quota_nblks(tpp, req->ip2, res->ip2_rtbcount,
			0, qflags, NULL);
	if (error)
		return error;

	return 0;
}

/* Enable the atomic file extent swap feature in the primary superblock. */
STATIC int
xfs_add_atomic_swap(
	struct xfs_mount	*mp)
{
	if (xfs_sb_version_hasatomicswap(&mp->m_sb))
		return 0;

	/*
	 * Atomic extent swapping is only supported on filesystems new enough
	 * to have reflink or rmap support enabled, and only if the filesystem
	 * isn't configured with realtime support.
	 */
	if (!xfs_sb_version_canatomicswap(&mp->m_sb) ||
	    xfs_sb_version_hasrealtime(&mp->m_sb))
		return -EOPNOTSUPP;

	xfs_warn(mp,
 "EXPERIMENTAL atomic file range swap feature added. Use at your own risk!");
	return xfs_add_incompat_log_feature(mp,
			XFS_SB_FEAT_INCOMPAT_LOG_ATOMIC_SWAP);
}

/* Decide if we can use log-assisted extent swapping (aka xfs_swapext()). */
static inline bool
xfs_xchg_use_log_items(
	struct xfs_mount		*mp,
	const struct file_xchg_range	*fxr)
{
	/*
	 * By definition, we can always use xfs_swapext() if atomic extent
	 * swapping is enabled.
	 */
	if (xfs_sb_version_hasatomicswap(&mp->m_sb))
		return true;

	/*
	 * If the user requested a non-atomic swap but our filesystem supports
	 * the deferred bmap log intent items that log-assisted swapping builds
	 * upon, then we're also ok.
	 */
	if ((fxr->flags & FILE_XCHG_RANGE_NONATOMIC) &&
	    xfs_sb_version_canatomicswap(&mp->m_sb))
		return true;

	return false;
}

/* Decide if we can use the old data fork exchange code. */
static inline bool
xfs_xchg_use_forkswap(
	const struct file_xchg_range	*fxr,
	struct xfs_inode		*ip1,
	struct xfs_inode		*ip2)
{
	return	(fxr->flags & FILE_XCHG_RANGE_NONATOMIC) &&
		(fxr->flags & FILE_XCHG_RANGE_FULL_FILES) &&
		!(fxr->flags & FILE_XCHG_RANGE_TO_EOF) &&
		fxr->file1_offset == 0 && fxr->file2_offset == 0 &&
		fxr->length == ip1->i_d.di_size &&
		fxr->length == ip2->i_d.di_size;
}

/* Exchange the contents of two files. */
int
xfs_xchg_range(
	struct xfs_inode		*ip1,
	struct xfs_inode		*ip2,
	const struct file_xchg_range	*fxr,
	unsigned int			private_flags)
{
	struct xfs_swapext_req		req = {
		.ip1			= ip1,
		.ip2			= ip2,
		.whichfork		= XFS_DATA_FORK,
	};
	struct xchg_quota_retries	qretry = { };
	struct xfs_swapext_res		res;
	struct xfs_mount		*mp = ip1->i_mount;
	struct xfs_trans		*tp;
	loff_t				req_len;
	int				error;

	if (fxr->flags & FILE_XCHG_RANGE_TO_EOF)
		req.flags |= XFS_SWAPEXT_SET_SIZES;
	if (fxr->flags & FILE_XCHG_RANGE_SKIP_FILE1_HOLES)
		req.flags |= XFS_SWAPEXT_SKIP_FILE1_HOLES;

	req.startoff1 = XFS_B_TO_FSBT(mp, fxr->file1_offset);
	req.startoff2 = XFS_B_TO_FSBT(mp, fxr->file2_offset);

	/*
	 * Round the request length up to the nearest fundamental unit of
	 * allocation.  The prep function already checked that the request
	 * offsets and length in @fxr are safe to round up.
	 */
	req_len = round_up(fxr->length, xfs_inode_alloc_unitsize(ip2));
	req.blockcount = XFS_B_TO_FSB(mp, req_len);

	/*
	 * Cancel CoW fork preallocations for the ranges of both files.  The
	 * prep function should have flushed all the dirty data, so the only
	 * extents remaining should be speculative.
	 */
	if (xfs_inode_has_cow_data(ip1)) {
		error = xfs_reflink_cancel_cow_range(ip1, fxr->file1_offset,
				fxr->length, true);
		if (error)
			return error;
	}

	if (xfs_inode_has_cow_data(ip2)) {
		error = xfs_reflink_cancel_cow_range(ip2, fxr->file2_offset,
				fxr->length, true);
		if (error)
			return error;
	}

	error = xfs_xchg_range_estimate(&req, &res);
	if (error)
		return error;

	/*
	 * If the caller wanted atomic swap, make sure the feature bit is
	 * turned on and ready to go.
	 */
	if (!(fxr->flags & FILE_XCHG_RANGE_NONATOMIC)) {
		error = xfs_add_atomic_swap(mp);
		if (error)
			return error;
	}

retry:
	/* Allocate the transaction, lock the inodes, and join them. */
	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, res.resblks, 0,
			XFS_TRANS_RES_FDBLKS, &tp);
	if (error)
		return error;

	xfs_xchg_range_ilock(tp, ip1, ip2);

	trace_xfs_swap_extent_before(ip2, 0);
	trace_xfs_swap_extent_before(ip1, 1);

	/*
	 * Do all of the inputs checking that we can only do once we've taken
	 * both ILOCKs.
	 */
	error = generic_xchg_file_range_check_fresh(VFS_I(ip1), VFS_I(ip2),
			fxr);
	if (error)
		goto out_trans_cancel;

	error = xfs_swapext_check_extents(mp, &req);
	if (error)
		goto out_trans_cancel;

	/*
	 * Reserve ourselves some quota if any of them are in enforcing mode.
	 * In theory we only need enough to satisfy the change in the number
	 * of blocks between the two ranges being remapped.
	 */
	error = xfs_xchg_range_reserve_quota(&tp, &req, &res, &qretry);
	if (error)
		return error;
	if (xchg_quota_want_retry(&qretry))
		goto retry;

	if (xfs_xchg_use_log_items(mp, fxr)) {
		/* Exchange the file contents by swapping the block mappings. */

		/* If we got this far on a dry run, all parameters are ok. */
		if (fxr->flags & FILE_XCHG_RANGE_DRY_RUN)
			goto out_trans_cancel;

		/* Update the mtime and ctime of both files. */
		if (private_flags & XFS_XCHG_RANGE_UPD_CMTIME1)
			xfs_trans_ichgtime(tp, ip1,
					XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
		if (private_flags & XFS_XCHG_RANGE_UPD_CMTIME2)
			xfs_trans_ichgtime(tp, ip2,
					XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);

		error = xfs_swapext(&tp, &req);
	} else if (xfs_xchg_use_forkswap(fxr, ip1, ip2)) {
		/*
		 * Exchange the file contents by using the old bmap fork
		 * exchange code, if we're a defrag tool doing a full file
		 * swap.
		 */
		error = xfs_swap_extents_check_format(ip2, ip1);
		if (error) {
			xfs_notice(mp,
		"%s: inode 0x%llx format is incompatible for exchanging.",
					__func__, ip2->i_ino);
			goto out_trans_cancel;
		}

		/* If we got this far on a dry run, all parameters are ok. */
		if (fxr->flags & FILE_XCHG_RANGE_DRY_RUN)
			goto out_trans_cancel;

		/* Update the mtime and ctime of both files. */
		if (private_flags & XFS_XCHG_RANGE_UPD_CMTIME1)
			xfs_trans_ichgtime(tp, ip1,
					XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
		if (private_flags & XFS_XCHG_RANGE_UPD_CMTIME2)
			xfs_trans_ichgtime(tp, ip2,
					XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);

		error = xfs_swap_extent_forks(&tp, &req);
	} else {
		/* We cannot exchange the file contents. */
		error = -EOPNOTSUPP;
	}
	if (error)
		goto out_trans_cancel;

	/*
	 * If the caller wanted us to exchange the contents of two complete
	 * files of unequal length, exchange the incore sizes now.  This should
	 * be safe because we flushed both files' page caches and moved all the
	 * post-eof extents, so there should not be anything to zero.
	 */
	if (fxr->flags & FILE_XCHG_RANGE_TO_EOF) {
		loff_t	temp;

		temp = i_size_read(VFS_I(ip2));
		i_size_write(VFS_I(ip2), i_size_read(VFS_I(ip1)));
		i_size_write(VFS_I(ip1), temp);
	}

	/* Relog the inodes to keep transactions moving forward. */
	xfs_trans_log_inode(tp, ip1, XFS_ILOG_CORE);
	xfs_trans_log_inode(tp, ip2, XFS_ILOG_CORE);

	/*
	 * Force the log to persist metadata updates if the caller or the
	 * administrator requires this.  The VFS prep function already flushed
	 * the relevant parts of the page cache.
	 */
	if ((mp->m_flags & XFS_MOUNT_WSYNC) ||
	    (fxr->flags & FILE_XCHG_RANGE_FSYNC))
		xfs_trans_set_sync(tp);

	error = xfs_trans_commit(tp);

	trace_xfs_swap_extent_after(ip2, 0);
	trace_xfs_swap_extent_after(ip1, 1);

out_unlock:
	xfs_xchg_range_iunlock(ip1, ip2);
	return error;

out_trans_cancel:
	xfs_trans_cancel(tp);
	goto out_unlock;
}
