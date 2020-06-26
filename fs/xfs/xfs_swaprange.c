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
#include "xfs_quota.h"
#include "xfs_bmap_util.h"
#include "xfs_reflink.h"
#include "xfs_trace.h"
#include "xfs_swapext.h"
#include "xfs_swaprange.h"

/* Lock (and optionally join) two inodes for an extent swap operation. */
void
xfs_swap_range_ilock(
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

/* Unlock two inodes after an extent swap operation. */
void
xfs_swap_range_iunlock(
	struct xfs_inode	*ip1,
	struct xfs_inode	*ip2)
{
	if (ip2 != ip1)
		xfs_iunlock(ip2, XFS_ILOCK_EXCL);
	xfs_iunlock(ip1, XFS_ILOCK_EXCL);
}

/*
 * Estimate the resource requirements to swap ranges between the two files.
 * The caller is required to hold the IOLOCK and the MMAPLOCK and to have
 * flushed both inodes' pagecache and active directios.
 */
int
xfs_swap_range_estimate(
	const struct xfs_swapext_req	*req,
	struct xfs_swapext_res		*res)
{
	int				error;

	xfs_swap_range_ilock(NULL, req->ip1, req->ip2);
	error = xfs_swapext_estimate(req, res);
	xfs_swap_range_iunlock(req->ip1, req->ip2);
	return error;
}

/* Prepare two files to have their data swapped. */
int
xfs_swap_range_prep(
	struct file		*file1,
	struct file		*file2,
	struct file_swap_range	*fsr)
{
	struct xfs_inode	*ip1 = XFS_I(file_inode(file1));
	struct xfs_inode	*ip2 = XFS_I(file_inode(file2));
	int			ret;

	/* Verify both files are either real-time or non-realtime */
	if (XFS_IS_REALTIME_INODE(ip1) != XFS_IS_REALTIME_INODE(ip2))
		return -EINVAL;

	ret = generic_swap_file_range_prep(file1, file2, fsr);
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
	ret = xfs_flush_unmap_range(ip2, fsr->file2_offset, fsr->length);
	if (ret)
		return ret;
	return xfs_flush_unmap_range(ip1, fsr->file1_offset, fsr->length);
}

/* Make a particular type of quota reservation. */
STATIC int
xfs_swap_range_reserve_quota_blocks(
	struct xfs_trans		*tp,
	const struct xfs_swapext_req	*req,
	xfs_filblks_t			ip1_mapped,
	xfs_filblks_t			ip2_mapped,
	unsigned int			qmopts)
{
	int				error;

	/*
	 * For each file, compute the net gain in the number of blocks that
	 * will be mapped into that file and reserve that much quota.  The
	 * quota counts must be able to absorb at least that much space.
	 */
	if (ip2_mapped > ip1_mapped) {
		error = xfs_trans_reserve_quota_nblks(tp, req->ip1,
				ip2_mapped - ip1_mapped, 0,
				qmopts);
		if (error)
			return error;
	}

	if (ip1_mapped > ip2_mapped) {
		error = xfs_trans_reserve_quota_nblks(tp, req->ip2,
				ip1_mapped - ip2_mapped, 0,
				qmopts);
		if (error)
			return error;
	}

	/*
	 * For each file, forcibly reserve the gross gain in mapped blocks so
	 * that we don't trip over any quota block reservation assertions.
	 * We must reserve the gross gain because the quota code subtracts from
	 * bcount the number of blocks that we unmap; it does not add that
	 * quantity back to the quota block reservation.
	 */
	error = xfs_trans_reserve_quota_nblks(tp, req->ip1, ip1_mapped, 0,
			XFS_QMOPT_FORCE_RES | qmopts);
	if (error)
		return error;

	return xfs_trans_reserve_quota_nblks(tp, req->ip2, ip2_mapped, 0,
			XFS_QMOPT_FORCE_RES | qmopts);
}

/*
 * Obtain a quota reservation to make sure we don't hit EDQUOT.  We can skip
 * this if quota enforcement is disabled or if both inodes' dquots are the
 * same.
 */
STATIC int
xfs_swap_range_reserve_quota(
	struct xfs_trans		*tp,
	const struct xfs_swapext_req	*req,
	const struct xfs_swapext_res	*res)
{
	int				error;

	/*
	 * Don't bother with a quota reservation if we're not enforcing them
	 * or the two inodes have the same dquots.
	 */
	if (!XFS_IS_QUOTA_ON(tp->t_mountp) ||
	    (req->ip1->i_udquot == req->ip2->i_udquot &&
	     req->ip1->i_gdquot == req->ip2->i_gdquot &&
	     req->ip1->i_pdquot == req->ip2->i_pdquot))
		return 0;

	error = xfs_swap_range_reserve_quota_blocks(tp, req, res->ip1_bcount,
			res->ip2_bcount, XFS_QMOPT_RES_REGBLKS);
	if (error)
		return error;
	return xfs_swap_range_reserve_quota_blocks(tp, req, res->ip1_rtbcount,
			res->ip2_rtbcount, XFS_QMOPT_RES_RTBLKS);
}

/* Decide if we can use xfs_swapext(). */
static inline bool
xfs_swapext_use_deferred(
	struct xfs_mount		*mp,
	const struct file_swap_range	*fsr)
{
	/* Fully recoverable extent swapping? Yes! */
	if (xfs_sb_version_hasatomicswap(&mp->m_sb))
		return true;

	/* Non-atomic swap requested and we have the deferred bmap log items */
	if ((fsr->flags & FILE_SWAP_RANGE_NONATOMIC) &&
	    (xfs_sb_version_hasreflink(&mp->m_sb) ||
	     xfs_sb_version_hasrmapbt(&mp->m_sb)))
		return true;

	return false;
}

/* Decide if we can use the old data fork exchange code. */
static inline bool
xfs_swapext_use_fork_exchange(
	const struct file_swap_range	*fsr,
	struct xfs_inode		*ip1,
	struct xfs_inode		*ip2)
{
	return	(fsr->flags & FILE_SWAP_RANGE_NONATOMIC) &&
		(fsr->flags & FILE_SWAP_RANGE_FULL_FILES) &&
		!(fsr->flags & FILE_SWAP_RANGE_TO_EOF) &&
		fsr->file1_offset == 0 && fsr->file2_offset == 0 &&
		fsr->length == ip1->i_d.di_size &&
		fsr->length == ip2->i_d.di_size;
}

/* Swap parts of two files. */
int
xfs_swap_range(
	struct xfs_inode	*ip1,
	struct xfs_inode	*ip2,
	const struct file_swap_range *fsr,
	unsigned int		private_flags)
{
	struct xfs_swapext_req	req = {
		.ip1		= ip1,
		.ip2		= ip2,
		.whichfork	= XFS_DATA_FORK,
	};
	struct xfs_swapext_res	res;
	struct xfs_mount	*mp = ip1->i_mount;
	struct xfs_trans	*tp;
	int			error;

	req.startoff1 = XFS_B_TO_FSBT(mp, fsr->file1_offset);
	req.startoff2 = XFS_B_TO_FSBT(mp, fsr->file2_offset);
	req.blockcount = XFS_B_TO_FSB(mp, fsr->length);

	/*
	 * Cancel CoW fork preallocations for the ranges of both files.  The
	 * prep function should have flushed all the dirty data, so the only
	 * extents remaining should be speculative.
	 */
	if (xfs_inode_has_cow_data(ip1)) {
		error = xfs_reflink_cancel_cow_range(ip1, fsr->file1_offset,
				fsr->length, true);
		if (error)
			return error;
	}

	if (xfs_inode_has_cow_data(ip2)) {
		error = xfs_reflink_cancel_cow_range(ip2, fsr->file2_offset,
				fsr->length, true);
		if (error)
			return error;
	}

	error = xfs_swap_range_estimate(&req, &res);
	if (error)
		return error;

	/* Allocate the transaction, lock the inodes, and join them. */
	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, res.resblks, 0, 0,
			&tp);
	if (error)
		return error;

	xfs_swap_range_ilock(tp, ip1, ip2);

	trace_xfs_swap_extent_before(ip2, 0);
	trace_xfs_swap_extent_before(ip1, 1);

	/*
	 * Do all of the inputs checking that we can only do once we've taken
	 * both ILOCKs.
	 */
	error = generic_swap_file_range_check_fresh(VFS_I(ip1), VFS_I(ip2),
			fsr);
	if (error)
		goto out_trans_cancel;

	if (ip1->i_df.if_format == XFS_DINODE_FMT_LOCAL ||
	    ip2->i_df.if_format == XFS_DINODE_FMT_LOCAL) {
		error = -EINVAL;
		goto out_trans_cancel;
	}

	/*
	 * Reserve ourselves some quota if any of them are in enforcing mode.
	 * In theory we only need enough to satisfy the change in the number
	 * of blocks between the two ranges being remapped.
	 */
	error = xfs_swap_range_reserve_quota(tp, &req, &res);
	if (error)
		goto out_trans_cancel;

	if (fsr->flags & FILE_SWAP_RANGE_TO_EOF)
		req.flags |= XFS_SWAPEXT_SET_SIZES;

	/* Perform the file range swap... */
	if (xfs_swapext_use_deferred(mp, fsr)) {
		/* If we got this far on a dry run, all parameters are ok. */
		if (fsr->flags & FILE_SWAP_RANGE_DRY_RUN)
			goto out_trans_cancel;

		/* Update the mtime and ctime of both files. */
		if (private_flags & XFS_SWAP_RANGE_UPD_CMTIME1)
			xfs_trans_ichgtime(tp, ip1,
					XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
		if (private_flags & XFS_SWAP_RANGE_UPD_CMTIME2)
			xfs_trans_ichgtime(tp, ip2,
					XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);

		/* ...by using the deferred op swapext, since it's available. */
		error = xfs_swapext(&tp, &req);
	} else if (xfs_swapext_use_fork_exchange(fsr, ip1, ip2)) {
		/*
		 * ...by using the old bmap fork exchange code, if we're a
		 * defrag tool doing a full file swap.
		 */
		error = xfs_swap_extents_check_format(ip2, ip1);
		if (error) {
			xfs_notice(mp,
		"%s: inode 0x%llx format is incompatible for exchanging.",
					__func__, ip2->i_ino);
			goto out_trans_cancel;
		}

		/* If we got this far on a dry run, all parameters are ok. */
		if (fsr->flags & FILE_SWAP_RANGE_DRY_RUN)
			goto out_trans_cancel;

		/* Update the mtime and ctime of both files. */
		if (private_flags & XFS_SWAP_RANGE_UPD_CMTIME1)
			xfs_trans_ichgtime(tp, ip1,
					XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
		if (private_flags & XFS_SWAP_RANGE_UPD_CMTIME2)
			xfs_trans_ichgtime(tp, ip2,
					XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);

		error = xfs_swap_extent_forks(&tp, &req);
	} else {
		/* ...or not at all, because we cannot do it. */
		error = -EOPNOTSUPP;
	}
	if (error)
		goto out_trans_cancel;

	/*
	 * If the caller wanted us to swap two complete files of unequal
	 * length, swap the incore sizes now.  This should be safe because we
	 * flushed both files' page caches and moved all the post-eof extents,
	 * so there should not be anything to zero.
	 */
	if (fsr->flags & FILE_SWAP_RANGE_TO_EOF) {
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
	    (fsr->flags & FILE_SWAP_RANGE_FSYNC))
		xfs_trans_set_sync(tp);

	error = xfs_trans_commit(tp);

	trace_xfs_swap_extent_after(ip2, 0);
	trace_xfs_swap_extent_after(ip1, 1);

out_unlock:
	xfs_swap_range_iunlock(ip1, ip2);
	return error;

out_trans_cancel:
	xfs_trans_cancel(tp);
	goto out_unlock;
}
