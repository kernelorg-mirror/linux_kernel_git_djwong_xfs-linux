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
#include "xfs_xchgrange.h"
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

	ret = -EOPNOTSUPP; /* XXX call out to xfs code */
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
