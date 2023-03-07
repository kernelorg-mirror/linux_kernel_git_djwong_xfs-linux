// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
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
#include "xfs_icache.h"
#include "xfs_log.h"
#include <linux/fsnotify.h>

/*
 * Generic code for exchanging ranges of two files via XFS_IOC_EXCHANGE_RANGE.
 * This part does not deal with XFS-specific data structures, and may some day
 * be ported to the VFS.
 *
 * The goal is to exchange fxr.length bytes starting at fxr.file1_offset in
 * file1 with the same number of bytes starting at fxr.file2_offset in file2.
 * Implementations must call xfs_exch_range_prep to prepare the two files
 * prior to taking locks; they must call xfs_exch_range_check_fresh once
 * the inode is locked to abort the call if file2 has changed; and they must
 * update the inode change and mod times of both files as part of the metadata
 * update.  The timestamp updates must be done atomically as part of the data
 * exchange operation to ensure correctness of the freshness check.
 */

/*
 * Check that both files' metadata agree with the snapshot that we took for
 * the range exchange request.
 *
 * This should be called after the filesystem has locked /all/ inode metadata
 * against modification.
 */
STATIC int
xfs_exch_range_check_fresh(
	struct inode			*inode2,
	const struct xfs_exch_range	*fxr)
{
	struct timespec64		ctime = inode_get_ctime(inode2);

	/* Check that file2 hasn't otherwise been modified. */
	if ((fxr->flags & XFS_EXCH_RANGE_FILE2_FRESH) &&
	    (fxr->file2_ino        != inode2->i_ino ||
	     fxr->file2_ctime      != ctime.tv_sec  ||
	     fxr->file2_ctime_nsec != ctime.tv_nsec ||
	     fxr->file2_mtime      != inode2->i_mtime.tv_sec  ||
	     fxr->file2_mtime_nsec != inode2->i_mtime.tv_nsec))
		return -EBUSY;

	return 0;
}

/* Performs necessary checks before doing a range exchange. */
STATIC int
xfs_exch_range_checks(
	struct file		*file1,
	struct file		*file2,
	struct xfs_exch_range	*fxr,
	unsigned int		blocksize)
{
	struct inode		*inode1 = file1->f_mapping->host;
	struct inode		*inode2 = file2->f_mapping->host;
	uint64_t		blkmask = blocksize - 1;
	int64_t			test_len;
	uint64_t		blen;
	loff_t			size1, size2;
	int			error;

	/* Don't touch certain kinds of inodes */
	if (IS_IMMUTABLE(inode1) || IS_IMMUTABLE(inode2))
		return -EPERM;
	if (IS_SWAPFILE(inode1) || IS_SWAPFILE(inode2))
		return -ETXTBSY;

	size1 = i_size_read(inode1);
	size2 = i_size_read(inode2);

	/* Ranges cannot start after EOF. */
	if (fxr->file1_offset > size1 || fxr->file2_offset > size2)
		return -EINVAL;

	/*
	 * If the caller asked for full files, check that the offset/length
	 * values cover all of both files.
	 */
	if ((fxr->flags & XFS_EXCH_RANGE_FULL_FILES) &&
	    (fxr->file1_offset != 0 || fxr->file2_offset != 0 ||
	     fxr->length != size1 || fxr->length != size2))
		return -EDOM;

	/*
	 * If the caller said to exchange to EOF, we set the length of the
	 * request large enough to cover everything to the end of both files.
	 */
	if (fxr->flags & XFS_EXCH_RANGE_TO_EOF)
		fxr->length = max_t(int64_t, size1 - fxr->file1_offset,
					     size2 - fxr->file2_offset);

	/* The start of both ranges must be aligned to an fs block. */
	if (!IS_ALIGNED(fxr->file1_offset, blocksize) ||
	    !IS_ALIGNED(fxr->file2_offset, blocksize))
		return -EINVAL;

	/* Ensure offsets don't wrap. */
	if (fxr->file1_offset + fxr->length < fxr->file1_offset ||
	    fxr->file2_offset + fxr->length < fxr->file2_offset)
		return -EINVAL;

	/*
	 * We require both ranges to be within EOF, unless we're exchanging
	 * to EOF.  xfs_xchg_range_prep already checked that both
	 * fxr->file1_offset and fxr->file2_offset are within EOF.
	 */
	if (!(fxr->flags & XFS_EXCH_RANGE_TO_EOF) &&
	    (fxr->file1_offset + fxr->length > size1 ||
	     fxr->file2_offset + fxr->length > size2))
		return -EINVAL;

	/*
	 * Make sure we don't hit any file size limits.  If we hit any size
	 * limits such that test_length was adjusted, we abort the whole
	 * operation.
	 */
	test_len = fxr->length;
	error = generic_write_check_limits(file2, fxr->file2_offset, &test_len);
	if (error)
		return error;
	error = generic_write_check_limits(file1, fxr->file1_offset, &test_len);
	if (error)
		return error;
	if (test_len != fxr->length)
		return -EINVAL;

	/*
	 * If the user wanted us to exchange up to the infile's EOF, round up
	 * to the next block boundary for this check.  Do the same for the
	 * outfile.
	 *
	 * Otherwise, reject the range length if it's not block aligned.  We
	 * already confirmed the starting offsets' block alignment.
	 */
	if (fxr->file1_offset + fxr->length == size1)
		blen = ALIGN(size1, blocksize) - fxr->file1_offset;
	else if (fxr->file2_offset + fxr->length == size2)
		blen = ALIGN(size2, blocksize) - fxr->file2_offset;
	else if (!IS_ALIGNED(fxr->length, blocksize))
		return -EINVAL;
	else
		blen = fxr->length;

	/* Don't allow overlapped exchanges within the same file. */
	if (inode1 == inode2 &&
	    fxr->file2_offset + blen > fxr->file1_offset &&
	    fxr->file1_offset + blen > fxr->file2_offset)
		return -EINVAL;

	/* If we already failed the freshness check, we're done. */
	error = xfs_exch_range_check_fresh(inode2, fxr);
	if (error)
		return error;

	/*
	 * Ensure that we don't exchange a partial EOF block into the middle of
	 * another file.
	 */
	if ((fxr->length & blkmask) == 0)
		return 0;

	blen = fxr->length;
	if (fxr->file2_offset + blen < size2)
		blen &= ~blkmask;

	if (fxr->file1_offset + blen < size1)
		blen &= ~blkmask;

	return blen == fxr->length ? 0 : -EINVAL;
}

/*
 * Check that the two inodes are eligible for range exchanges, the ranges make
 * sense, and then flush all dirty data.  Caller must ensure that the inodes
 * have been locked against any other modifications.
 */
int
xfs_exch_range_prep(
	struct file		*file1,
	struct file		*file2,
	struct xfs_exch_range	*fxr,
	unsigned int		blocksize)
{
	struct inode		*inode1 = file_inode(file1);
	struct inode		*inode2 = file_inode(file2);
	bool			same_inode = (inode1 == inode2);
	int			error;

	/* Check that we don't violate system file offset limits. */
	error = xfs_exch_range_checks(file1, file2, fxr, blocksize);
	if (error || fxr->length == 0)
		return error;

	/* Wait for the completion of any pending IOs on both files */
	inode_dio_wait(inode1);
	if (!same_inode)
		inode_dio_wait(inode2);

	error = filemap_write_and_wait_range(inode1->i_mapping,
			fxr->file1_offset,
			fxr->file1_offset + fxr->length - 1);
	if (error)
		return error;

	error = filemap_write_and_wait_range(inode2->i_mapping,
			fxr->file2_offset,
			fxr->file2_offset + fxr->length - 1);
	if (error)
		return error;

	/*
	 * If the files or inodes involved require synchronous writes, amend
	 * the request to force the filesystem to flush all data and metadata
	 * to disk after the operation completes.
	 */
	if (((file1->f_flags | file2->f_flags) & (__O_SYNC | O_DSYNC)) ||
	    IS_SYNC(inode1) || IS_SYNC(inode2))
		fxr->flags |= XFS_EXCH_RANGE_FSYNC;

	return 0;
}

/*
 * Finish a range exchange operation, if it was successful.  Caller must ensure
 * that the inodes are still locked against any other modifications.
 */
int
xfs_exch_range_finish(
	struct file		*file1,
	struct file		*file2)
{
	int			error;

	error = file_remove_privs(file1);
	if (error)
		return error;
	if (file_inode(file1) == file_inode(file2))
		return 0;

	return file_remove_privs(file2);
}

/* Decide if it's ok to remap the selected range of a given file. */
STATIC int
xfs_exch_range_verify_area(
	struct file		*file,
	loff_t			pos,
	struct xfs_exch_range	*fxr)
{
	int64_t			len = fxr->length;

	if (pos < 0)
		return -EINVAL;

	if (fxr->flags & XFS_EXCH_RANGE_TO_EOF)
		len = min_t(int64_t, len, i_size_read(file_inode(file)) - pos);
	return remap_verify_area(file, pos, len, true);
}

/* Prepare for and exchange parts of two files. */
static inline int
__xfs_exch_range(
	struct file		*file1,
	struct file		*file2,
	struct xfs_exch_range	*fxr)
{
	struct inode		*inode1 = file_inode(file1);
	struct inode		*inode2 = file_inode(file2);
	int			ret;

	if ((fxr->flags & ~XFS_EXCH_RANGE_ALL_FLAGS) ||
	    memchr_inv(&fxr->pad, 0, sizeof(fxr->pad)))
		return -EINVAL;

	if ((fxr->flags & XFS_EXCH_RANGE_FULL_FILES) &&
	    (fxr->flags & XFS_EXCH_RANGE_TO_EOF))
		return -EINVAL;

	/*
	 * The ioctl enforces that src and dest files are on the same mount.
	 * However, they only need to be on the same file system.
	 */
	if (inode1->i_sb != inode2->i_sb)
		return -EXDEV;

	/* This only works for regular files. */
	if (S_ISDIR(inode1->i_mode) || S_ISDIR(inode2->i_mode))
		return -EISDIR;
	if (!S_ISREG(inode1->i_mode) || !S_ISREG(inode2->i_mode))
		return -EINVAL;

	ret = generic_file_rw_checks(file1, file2);
	if (ret < 0)
		return ret;

	ret = generic_file_rw_checks(file2, file1);
	if (ret < 0)
		return ret;

	ret = xfs_exch_range_verify_area(file1, fxr->file1_offset, fxr);
	if (ret)
		return ret;

	ret = xfs_exch_range_verify_area(file2, fxr->file2_offset, fxr);
	if (ret)
		return ret;

	ret = xfs_file_xchg_range(file1, file2, fxr);
	if (ret)
		return ret;

	fsnotify_modify(file1);
	if (file2 != file1)
		fsnotify_modify(file2);
	return 0;
}

/* Exchange parts of two files. */
int
xfs_exch_range(
	struct file		*file1,
	struct file		*file2,
	struct xfs_exch_range	*fxr)
{
	int			error;

	file_start_write(file2);
	error = __xfs_exch_range(file1, file2, fxr);
	file_end_write(file2);
	return error;
}

/* XFS-specific parts of XFS_IOC_EXCHANGE_RANGE */

/*
 * Exchanging ranges as a file operation.  This is the binding between the
 * VFS-level concepts and the XFS-specific implementation.
 */
int
xfs_file_xchg_range(
	struct file		*file1,
	struct file		*file2,
	struct xfs_exch_range	*fxr)
{
	struct inode		*inode1 = file_inode(file1);
	struct inode		*inode2 = file_inode(file2);
	struct xfs_inode	*ip1 = XFS_I(inode1);
	struct xfs_inode	*ip2 = XFS_I(inode2);
	struct xfs_mount	*mp = ip1->i_mount;
	unsigned int		priv_flags = 0;
	bool			use_logging = false;
	int			error;

	if (xfs_is_shutdown(mp))
		return -EIO;

	/* Update cmtime if the fd/inode don't forbid it. */
	if (likely(!(file1->f_mode & FMODE_NOCMTIME) && !IS_NOCMTIME(inode1)))
		priv_flags |= XFS_XCHG_RANGE_UPD_CMTIME1;
	if (likely(!(file2->f_mode & FMODE_NOCMTIME) && !IS_NOCMTIME(inode2)))
		priv_flags |= XFS_XCHG_RANGE_UPD_CMTIME2;

	/* Lock both files against IO */
	error = xfs_ilock2_io_mmap(ip1, ip2);
	if (error)
		goto out_err;

	/* Get permission to use log-assisted file content swaps. */
	error = xfs_xchg_range_grab_log_assist(mp,
			!(fxr->flags & XFS_EXCH_RANGE_NONATOMIC),
			&use_logging);
	if (error)
		goto out_unlock;
	if (use_logging)
		priv_flags |= XFS_XCHG_RANGE_LOGGED;

	/* Prepare and then exchange file contents. */
	error = xfs_xchg_range_prep(file1, file2, fxr);
	if (error)
		goto out_drop_feat;

	error = xfs_xchg_range(ip1, ip2, fxr, priv_flags);
	if (error)
		goto out_drop_feat;

	/*
	 * Finish the exchange by removing special file privileges like any
	 * other file write would do.  This may involve turning on support for
	 * logged xattrs if either file has security capabilities, which means
	 * xfs_xchg_range_grab_log_assist before xfs_attr_grab_log_assist.
	 */
	error = xfs_exch_range_finish(file1, file2);
	if (error)
		goto out_drop_feat;

out_drop_feat:
	if (use_logging)
		xfs_xchg_range_rele_log_assist(mp);
out_unlock:
	xfs_iunlock2_io_mmap(ip1, ip2);
out_err:
	if (error)
		trace_xfs_file_xchg_range_error(ip2, error, _RET_IP_);
	return error;
}

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
	struct xfs_swapext_req	*req)
{
	int			error;

	xfs_xchg_range_ilock(NULL, req->ip1, req->ip2);
	error = xfs_swapext_estimate(req);
	xfs_xchg_range_iunlock(req->ip1, req->ip2);
	return error;
}

/* Prepare two files to have their data exchanged. */
int
xfs_xchg_range_prep(
	struct file		*file1,
	struct file		*file2,
	struct xfs_exch_range	*fxr)
{
	struct xfs_inode	*ip1 = XFS_I(file_inode(file1));
	struct xfs_inode	*ip2 = XFS_I(file_inode(file2));
	int			error;

	trace_xfs_xchg_range_prep(ip1, fxr, ip2, 0);

	/* Verify both files are either real-time or non-realtime */
	if (XFS_IS_REALTIME_INODE(ip1) != XFS_IS_REALTIME_INODE(ip2))
		return -EINVAL;

	/*
	 * The alignment checks in the VFS helpers cannot deal with allocation
	 * units that are not powers of 2.  This can happen with the realtime
	 * volume if the extent size is set.  Note that alignment checks are
	 * skipped if FULL_FILES is set.
	 */
	if (!(fxr->flags & XFS_EXCH_RANGE_FULL_FILES) &&
	    !is_power_of_2(xfs_inode_alloc_unitsize(ip2)))
		return -EOPNOTSUPP;

	error = xfs_exch_range_prep(file1, file2, fxr,
			xfs_inode_alloc_unitsize(ip2));
	if (error || fxr->length == 0)
		return error;

	/* Attach dquots to both inodes before changing block maps. */
	error = xfs_qm_dqattach(ip2);
	if (error)
		return error;
	error = xfs_qm_dqattach(ip1);
	if (error)
		return error;

	trace_xfs_xchg_range_flush(ip1, fxr, ip2, 0);

	/* Flush the relevant ranges of both files. */
	error = xfs_flush_unmap_range(ip2, fxr->file2_offset, fxr->length);
	if (error)
		return error;
	error = xfs_flush_unmap_range(ip1, fxr->file1_offset, fxr->length);
	if (error)
		return error;

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

	return 0;
}

#define QRETRY_IP1	(0x1)
#define QRETRY_IP2	(0x2)

/*
 * Obtain a quota reservation to make sure we don't hit EDQUOT.  We can skip
 * this if quota enforcement is disabled or if both inodes' dquots are the
 * same.  The qretry structure must be initialized to zeroes before the first
 * call to this function.
 */
STATIC int
xfs_xchg_range_reserve_quota(
	struct xfs_trans		*tp,
	const struct xfs_swapext_req	*req,
	unsigned int			*qretry)
{
	int64_t				ddelta, rdelta;
	int				ip1_error = 0;
	int				error;

	/*
	 * Don't bother with a quota reservation if we're not enforcing them
	 * or the two inodes have the same dquots.
	 */
	if (!XFS_IS_QUOTA_ON(tp->t_mountp) || req->ip1 == req->ip2 ||
	    (req->ip1->i_udquot == req->ip2->i_udquot &&
	     req->ip1->i_gdquot == req->ip2->i_gdquot &&
	     req->ip1->i_pdquot == req->ip2->i_pdquot))
		return 0;

	*qretry = 0;

	/*
	 * For each file, compute the net gain in the number of regular blocks
	 * that will be mapped into that file and reserve that much quota.  The
	 * quota counts must be able to absorb at least that much space.
	 */
	ddelta = req->ip2_bcount - req->ip1_bcount;
	rdelta = req->ip2_rtbcount - req->ip1_rtbcount;
	if (ddelta > 0 || rdelta > 0) {
		error = xfs_trans_reserve_quota_nblks(tp, req->ip1,
				ddelta > 0 ? ddelta : 0,
				rdelta > 0 ? rdelta : 0,
				false);
		if (error == -EDQUOT || error == -ENOSPC) {
			/*
			 * Save this error and see what happens if we try to
			 * reserve quota for ip2.  Then report both.
			 */
			*qretry |= QRETRY_IP1;
			ip1_error = error;
			error = 0;
		}
		if (error)
			return error;
	}
	if (ddelta < 0 || rdelta < 0) {
		error = xfs_trans_reserve_quota_nblks(tp, req->ip2,
				ddelta < 0 ? -ddelta : 0,
				rdelta < 0 ? -rdelta : 0,
				false);
		if (error == -EDQUOT || error == -ENOSPC)
			*qretry |= QRETRY_IP2;
		if (error)
			return error;
	}
	if (ip1_error)
		return ip1_error;

	/*
	 * For each file, forcibly reserve the gross gain in mapped blocks so
	 * that we don't trip over any quota block reservation assertions.
	 * We must reserve the gross gain because the quota code subtracts from
	 * bcount the number of blocks that we unmap; it does not add that
	 * quantity back to the quota block reservation.
	 */
	error = xfs_trans_reserve_quota_nblks(tp, req->ip1, req->ip1_bcount,
			req->ip1_rtbcount, true);
	if (error)
		return error;

	return xfs_trans_reserve_quota_nblks(tp, req->ip2, req->ip2_bcount,
			req->ip2_rtbcount, true);
}

/*
 * Get permission to use log-assisted atomic exchange of file extents.
 *
 * Callers must hold the IOLOCK and MMAPLOCK of both files.  They must not be
 * running any transactions or hold any ILOCKS.  If @use_logging is set after a
 * successful return, callers must call xfs_xchg_range_rele_log_assist after
 * the exchange is completed.
 */
int
xfs_xchg_range_grab_log_assist(
	struct xfs_mount	*mp,
	bool			force,
	bool			*use_logging)
{
	int			error = 0;

	/*
	 * As a performance optimization, skip the log force and super write
	 * if the filesystem featureset already protects the swapext log items.
	 */
	if (xfs_swapext_can_use_without_log_assistance(mp)) {
		*use_logging = true;
		return 0;
	}

	/*
	 * Protect ourselves from an idle log clearing the atomic swapext
	 * log incompat feature bit.
	 */
	xlog_use_incompat_feat(mp->m_log, XLOG_INCOMPAT_FEAT_SWAPEXT);
	*use_logging = true;

	/*
	 * If log-assisted swapping is already enabled, the caller can use the
	 * log assisted swap functions with the log-incompat reference we got.
	 */
	if (xfs_sb_version_haslogswapext(&mp->m_sb))
		return 0;

	/*
	 * If the caller doesn't /require/ log-assisted swapping, drop the
	 * incore log-incompat feature protection and exit.  The caller will
	 * not be able to use log assisted swapping.
	 */
	if (!force)
		goto drop_incompat;

	/*
	 * Check if the filesystem featureset is new enough to set this log
	 * incompat feature bit.  Strictly speaking, the minimum requirement is
	 * a V5 filesystem for the superblock field, but we'll require bigtime
	 * to avoid having to deal with really old kernels.
	 */
	if (!xfs_has_bigtime(mp)) {
		error = -EOPNOTSUPP;
		goto drop_incompat;
	}

	error = xfs_add_incompat_log_feature(mp,
			XFS_SB_FEAT_INCOMPAT_LOG_SWAPEXT);
	if (error)
		goto drop_incompat;

	xfs_warn_mount(mp, XFS_OPSTATE_WARNED_SWAPEXT,
 "EXPERIMENTAL atomic file range swap feature in use. Use at your own risk!");

	return 0;
drop_incompat:
	xlog_drop_incompat_feat(mp->m_log, XLOG_INCOMPAT_FEAT_SWAPEXT);
	*use_logging = false;
	return error;
}

/* Release permission to use log-assisted extent swapping. */
void
xfs_xchg_range_rele_log_assist(
	struct xfs_mount	*mp)
{
	if (!xfs_swapext_can_use_without_log_assistance(mp))
		xlog_drop_incompat_feat(mp->m_log, XLOG_INCOMPAT_FEAT_SWAPEXT);
}

/*
 * Can we use xfs_swapext() to perform the exchange?
 *
 * The swapext state tracking mechanism uses deferred bmap log intent (BUI)
 * items to swap extents between file forks, and it /can/ track the overall
 * operation status over a file range using swapext log intent (SXI) items.
 */
static inline bool
xfs_xchg_use_swapext(
	struct xfs_mount	*mp,
	unsigned int		xchg_flags)
{
	/*
	 * If the caller got permission from the log to use SXI items, we will
	 * use xfs_swapext with both log items.
	 */
	if (xchg_flags & XFS_XCHG_RANGE_LOGGED)
		return true;

	/*
	 * If the caller didn't get permission to use SXI items, then userspace
	 * must have allowed non-atomic swap mode.  Use the state tracking in
	 * xfs_swapext to log BUI log items if the fs supports rmap or reflink.
	 */
	return xfs_swapext_supports_nonatomic(mp);
}

/*
 * Can we use the old data fork swapping to perform the exchange?
 *
 * Userspace must be asking for a full swap of two files with the same file
 * size and cannot require atomic mode.
 */
static inline bool
xfs_xchg_use_forkswap(
	const struct xfs_exch_range	*fxr,
	struct xfs_inode		*ip1,
	struct xfs_inode		*ip2)
{
	if (!(fxr->flags & XFS_EXCH_RANGE_NONATOMIC))
		return false;
	if (!(fxr->flags & XFS_EXCH_RANGE_FULL_FILES))
		return false;
	if (fxr->flags & XFS_EXCH_RANGE_TO_EOF)
		return false;
	if (fxr->file1_offset != 0 || fxr->file2_offset != 0)
		return false;
	if (fxr->length != ip1->i_disk_size)
		return false;
	if (fxr->length != ip2->i_disk_size)
		return false;
	return true;
}

enum xchg_strategy {
	SWAPEXT		= 1,	/* xfs_swapext() */
	FORKSWAP	= 2,	/* exchange forks */
};

/* Exchange the contents of two files. */
int
xfs_xchg_range(
	struct xfs_inode		*ip1,
	struct xfs_inode		*ip2,
	const struct xfs_exch_range	*fxr,
	unsigned int			xchg_flags)
{
	struct xfs_mount		*mp = ip1->i_mount;
	struct xfs_swapext_req		req = {
		.ip1			= ip1,
		.ip2			= ip2,
		.whichfork		= XFS_DATA_FORK,
		.startoff1		= XFS_B_TO_FSBT(mp, fxr->file1_offset),
		.startoff2		= XFS_B_TO_FSBT(mp, fxr->file2_offset),
		.blockcount		= XFS_B_TO_FSB(mp, fxr->length),
	};
	struct xfs_trans		*tp;
	unsigned int			qretry;
	unsigned int			flags = 0;
	bool				retried = false;
	enum xchg_strategy		strategy;
	int				error;

	trace_xfs_xchg_range(ip1, fxr, ip2, xchg_flags);

	if (fxr->flags & XFS_EXCH_RANGE_TO_EOF)
		req.req_flags |= XFS_SWAP_REQ_SET_SIZES;
	if (fxr->flags & XFS_EXCH_RANGE_FILE1_WRITTEN)
		req.req_flags |= XFS_SWAP_REQ_INO1_WRITTEN;
	if (xchg_flags & XFS_XCHG_RANGE_LOGGED)
		req.req_flags |= XFS_SWAP_REQ_LOGGED;

	error = xfs_xchg_range_estimate(&req);
	if (error)
		return error;

	/*
	 * We haven't decided which exchange strategy we want to use yet, but
	 * here we must choose if we want freed blocks during the swap to be
	 * added to the transaction block reservation (RES_FDBLKS) or freed
	 * into the global fdblocks.  The legacy fork swap mechanism doesn't
	 * free any blocks, so it doesn't require it.  It is also the only
	 * option that works for older filesystems.
	 *
	 * The bmap log intent items that were added with rmap and reflink can
	 * change the bmbt shape, so the intent-based swap strategies require
	 * us to set RES_FDBLKS.
	 */
	if (xfs_has_lazysbcount(mp))
		flags |= XFS_TRANS_RES_FDBLKS;

retry:
	/* Allocate the transaction, lock the inodes, and join them. */
	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, req.resblks, 0,
			flags, &tp);
	if (error)
		return error;

	xfs_xchg_range_ilock(tp, ip1, ip2);

	trace_xfs_swap_extent_before(ip2, 0);
	trace_xfs_swap_extent_before(ip1, 1);

	if (fxr->flags & XFS_EXCH_RANGE_FILE2_FRESH)
		trace_xfs_xchg_range_freshness(ip2, fxr);

	/*
	 * Now that we've excluded all other inode metadata changes by taking
	 * the ILOCK, repeat the freshness check.
	 */
	error = xfs_exch_range_check_fresh(VFS_I(ip2), fxr);
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
	error = xfs_xchg_range_reserve_quota(tp, &req, &qretry);
	if ((error == -EDQUOT || error == -ENOSPC) && !retried) {
		xfs_trans_cancel(tp);
		xfs_xchg_range_iunlock(ip1, ip2);
		if (qretry & QRETRY_IP1)
			xfs_blockgc_free_quota(ip1, 0);
		if (qretry & QRETRY_IP2)
			xfs_blockgc_free_quota(ip2, 0);
		retried = true;
		goto retry;
	}
	if (error)
		goto out_trans_cancel;

	if (xfs_xchg_use_swapext(mp, xchg_flags)) {
		/* Exchange the file contents with our fancy state tracking. */
		strategy = SWAPEXT;
	} else if (xfs_xchg_use_forkswap(fxr, ip1, ip2)) {
		/*
		 * Exchange the file contents by using the old bmap fork
		 * exchange code, if we're a defrag tool doing a full file
		 * swap.
		 */
		strategy = FORKSWAP;

		error = xfs_swap_extents_check_format(ip2, ip1);
		if (error) {
			xfs_notice(mp,
		"%s: inode 0x%llx format is incompatible for exchanging.",
					__func__, ip2->i_ino);
			goto out_trans_cancel;
		}
	} else {
		/* We cannot exchange the file contents. */
		error = -EOPNOTSUPP;
		goto out_trans_cancel;
	}

	/* If we got this far on a dry run, all parameters are ok. */
	if (fxr->flags & XFS_EXCH_RANGE_DRY_RUN)
		goto out_trans_cancel;

	/* Update the mtime and ctime of both files. */
	if (xchg_flags & XFS_XCHG_RANGE_UPD_CMTIME1)
		xfs_trans_ichgtime(tp, ip1,
				XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
	if (xchg_flags & XFS_XCHG_RANGE_UPD_CMTIME2)
		xfs_trans_ichgtime(tp, ip2,
				XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);

	switch (strategy) {
	case SWAPEXT:
		xfs_swapext(tp, &req);
		error = 0;
		break;
	case FORKSWAP:
		error = xfs_swap_extent_forks(&tp, &req);
		break;
	}
	if (error)
		goto out_trans_cancel;

	/*
	 * Force the log to persist metadata updates if the caller or the
	 * administrator requires this.  The VFS prep function already flushed
	 * the relevant parts of the page cache.
	 */
	if (xfs_has_wsync(mp) || (fxr->flags & XFS_EXCH_RANGE_FSYNC))
		xfs_trans_set_sync(tp);

	error = xfs_trans_commit(tp);

	trace_xfs_swap_extent_after(ip2, 0);
	trace_xfs_swap_extent_after(ip1, 1);

	if (error)
		goto out_unlock;

	/*
	 * If the caller wanted us to exchange the contents of two complete
	 * files of unequal length, exchange the incore sizes now.  This should
	 * be safe because we flushed both files' page caches, moved all the
	 * extents, and updated the ondisk sizes.
	 */
	if (fxr->flags & XFS_EXCH_RANGE_TO_EOF) {
		loff_t	temp;

		temp = i_size_read(VFS_I(ip2));
		i_size_write(VFS_I(ip2), i_size_read(VFS_I(ip1)));
		i_size_write(VFS_I(ip1), temp);
	}

out_unlock:
	xfs_xchg_range_iunlock(ip1, ip2);
	return error;

out_trans_cancel:
	xfs_trans_cancel(tp);
	goto out_unlock;
}
