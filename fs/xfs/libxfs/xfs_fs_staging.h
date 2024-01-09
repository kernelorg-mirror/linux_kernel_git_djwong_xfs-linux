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

#define XFS_IOC_RTGROUP_GEOMETRY _IOWR('X', 63, struct xfs_rtgroup_geometry)

#endif /* __XFS_FS_STAGING_H__ */
