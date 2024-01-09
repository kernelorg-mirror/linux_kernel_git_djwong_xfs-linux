/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_FS_STAGING_H__
#define __XFS_FS_STAGING_H__

/*
 * Experimental system calls, ioctls and data structures supporting them.
 * Nothing in here should be considered part of a stable interface of any kind.
 *
 * If you add an ioctl here, please leave a comment in xfs_fs.h marking it
 * reserved.  If you promote anything out of this file, please leave a comment
 * explaining where it went.
 */

/*
 * Exchange part of file1 with part of the file that this ioctl that is being
 * called against (which we'll call file2).  Filesystems must be able to
 * restart and complete the operation even after the system goes down.
 */
struct xfs_exch_range {
	__s64		file1_fd;
	__s64		file1_offset;	/* file1 offset, bytes */
	__s64		file2_offset;	/* file2 offset, bytes */
	__u64		length;		/* bytes to exchange */

	__u64		flags;		/* see XFS_EXCHRANGE_* below */

	__u64		pad;		/* must be zeroes */
};

/*
 * Using the same definition of file2 as struct xfs_exch_range, commit the
 * contents of file1 into file2 if file2 has the same inode number, mtime, and
 * ctime as the arguments provided to the call.  The old contents of file2 will
 * be moved to file1.
 *
 * Returns -EBUSY if there isn't an exact match for the file2 fields.
 *
 * Filesystems must be able to restart and complete the operation even after
 * the system goes down.
 */
struct xfs_commit_range {
	__s64		file1_fd;
	__s64		file1_offset;	/* file1 offset, bytes */
	__s64		file2_offset;	/* file2 offset, bytes */
	__s64		length;		/* bytes to exchange */

	__u64		flags;		/* see XFS_EXCHRANGE_* below */

	/* file2 metadata for freshness checks */
	__u64		file2_ino;	/* inode number */
	__s64		file2_mtime;	/* modification time */
	__s64		file2_ctime;	/* change time */
	__s32		file2_mtime_nsec; /* mod time, nsec */
	__s32		file2_ctime_nsec; /* change time, nsec */

	__u64		pad;		/* must be zeroes */
};

/*
 * Exchange file data all the way to the ends of both files, and then exchange
 * the file sizes.  This flag can be used to replace a file's contents with a
 * different amount of data.  length will be ignored.
 */
#define XFS_EXCHRANGE_TO_EOF		(1ULL << 0)

/* Flush all changes in file data and file metadata to disk before returning. */
#define XFS_EXCHRANGE_DSYNC		(1ULL << 1)

/* Dry run; do all the parameter verification but do not change anything. */
#define XFS_EXCHRANGE_DRY_RUN		(1ULL << 2)

/*
 * Exchange only the parts of the two files where the file allocation units
 * mapped to file1's range have been written to.  This can accelerate
 * scatter-gather atomic writes with a temp file if all writes are aligned to
 * the file allocation unit.
 */
#define XFS_EXCHRANGE_FILE1_WRITTEN	(1ULL << 3)

#define XFS_EXCHRANGE_ALL_FLAGS		(XFS_EXCHRANGE_TO_EOF | \
					 XFS_EXCHRANGE_DSYNC | \
					 XFS_EXCHRANGE_DRY_RUN | \
					 XFS_EXCHRANGE_FILE1_WRITTEN)

#define XFS_IOC_EXCHANGE_RANGE	_IOWR('X', 129, struct xfs_exch_range)
#define XFS_IOC_COMMIT_RANGE	_IOWR('X', 129, struct xfs_commit_range)

#endif /* __XFS_FS_STAGING_H__ */
