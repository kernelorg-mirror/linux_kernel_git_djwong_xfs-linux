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

/*
 * Output for XFS_IOC_RTGROUP_GEOMETRY
 */
struct xfs_rtgroup_geometry {
	__u32 rg_number;	/* i/o: rtgroup number */
	__u32 rg_length;	/* o: length in blocks */
	__u32 rg_sick;		/* o: sick things in ag */
	__u32 rg_checked;	/* o: checked metadata in ag */
	__u32 rg_flags;		/* i/o: flags for this ag */
	__u32 rg_pad;		/* o: zero */
	__u64 rg_reserved[13];	/* o: zero */
};
#define XFS_RTGROUP_GEOM_SICK_SUPER	(1 << 0)  /* superblock */
#define XFS_RTGROUP_GEOM_SICK_BITMAP	(1 << 1)  /* rtbitmap for this group */

#define XFS_IOC_RTGROUP_GEOMETRY _IOWR('X', 63, struct xfs_rtgroup_geometry)

#endif /* __XFS_FS_STAGING_H__ */
