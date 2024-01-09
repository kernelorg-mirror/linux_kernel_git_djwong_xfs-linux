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

/* Iterating parent pointers of files. */

/* return parents of the handle, not the open fd */
#define XFS_GETPARENTS_IFLAG_HANDLE	(1U << 0)

/* target was the root directory */
#define XFS_GETPARENTS_OFLAG_ROOT	(1U << 1)

/* Cursor is done iterating pptrs */
#define XFS_GETPARENTS_OFLAG_DONE	(1U << 2)

#define XFS_GETPARENTS_FLAG_ALL		(XFS_GETPARENTS_IFLAG_HANDLE | \
					 XFS_GETPARENTS_OFLAG_ROOT | \
					 XFS_GETPARENTS_OFLAG_DONE)

/* Get an inode parent pointer through ioctl */
struct xfs_getparents_rec {
	__u64		gpr_ino;	/* Inode number */
	__u32		gpr_gen;	/* Inode generation */
	__u32		gpr_pad;	/* Reserved */
	__u64		gpr_rsvd;	/* Reserved */
	__u8		gpr_name[];	/* File name and null terminator */
};

/* Iterate through an inodes parent pointers */
struct xfs_getparents {
	/* File handle, if XFS_GETPARENTS_IFLAG_HANDLE is set */
	struct xfs_handle		gp_handle;

	/*
	 * Structure to track progress in iterating the parent pointers.
	 * Must be initialized to zeroes before the first ioctl call, and
	 * not touched by callers after that.
	 */
	struct xfs_attrlist_cursor	gp_cursor;

	/* Operational flags: XFS_GETPARENTS_*FLAG* */
	__u32				gp_flags;

	/* Must be set to zero */
	__u32				gp_reserved;

	/* Size of the buffer in bytes, including this header */
	__u32				gp_bufsize;

	/* # of entries filled in (output) */
	__u32				gp_count;

	/* Must be set to zero */
	__u64				gp_reserved2[5];

	/* Byte offset of each record within the buffer */
	__u32				gp_offsets[];
};

static inline struct xfs_getparents_rec*
xfs_getparents_rec(
	struct xfs_getparents	*info,
	unsigned int		idx)
{
	return (struct xfs_getparents_rec *)((char *)info +
					     info->gp_offsets[idx]);
}

#define XFS_IOC_GETPARENTS	_IOWR('X', 62, struct xfs_getparents)

/* Vectored scrub calls to reduce the number of kernel transitions. */

struct xfs_scrub_vec {
	__u32 sv_type;		/* XFS_SCRUB_TYPE_* */
	__u32 sv_flags;		/* XFS_SCRUB_FLAGS_* */
	__s32 sv_ret;		/* 0 or a negative error code */
	__u32 sv_reserved;	/* must be zero */
};

/* Vectored metadata scrub control structure. */
struct xfs_scrub_vec_head {
	__u64 svh_ino;		/* inode number. */
	__u32 svh_gen;		/* inode generation. */
	__u32 svh_agno;		/* ag number. */
	__u32 svh_flags;	/* XFS_SCRUB_VEC_FLAGS_* */
	__u16 svh_rest_us;	/* wait this much time between vector items */
	__u16 svh_nr;		/* number of svh_vecs */
	__u64 svh_reserved;	/* must be zero */

	struct xfs_scrub_vec svh_vecs[];
};

#define XFS_SCRUB_VEC_FLAGS_ALL		(0)

static inline size_t sizeof_xfs_scrub_vec(unsigned int nr)
{
	return sizeof(struct xfs_scrub_vec_head) +
		nr * sizeof(struct xfs_scrub_vec);
}

#define XFS_IOC_SCRUBV_METADATA	_IOWR('X', 60, struct xfs_scrub_vec_head)

#endif /* __XFS_FS_STAGING_H__ */
