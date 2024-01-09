/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_EXCHRANGE_H__
#define __XFS_EXCHRANGE_H__

bool xfs_exchrange_possible(struct xfs_mount *mp);

/* Update the mtime/cmtime of file1 and file2 */
#define __XFS_EXCHRANGE_UPD_CMTIME1	(1ULL << 63)
#define __XFS_EXCHRANGE_UPD_CMTIME2	(1ULL << 62)

/* Freshness check required */
#define __XFS_EXCHRANGE_CHECK_FRESH2	(1ULL << 61)

#define XFS_EXCHRANGE_PRIVATE_FLAGS	(__XFS_EXCHRANGE_UPD_CMTIME1 | \
					 __XFS_EXCHRANGE_UPD_CMTIME2 | \
					 __XFS_EXCHRANGE_CHECK_FRESH2)

struct xfs_exchrange {
	struct file		*file1;
	struct file		*file2;

	loff_t			file1_offset;
	loff_t			file2_offset;
	u64			length;

	u64			flags;	/* XFS_EXCHRANGE flags */

	/* file2 metadata for freshness checks if file1_ino != 0 */
	u64			file2_ino;
	struct timespec64	file2_mtime;
	struct timespec64	file2_ctime;
};

int xfs_exchange_range(struct xfs_exchrange *fxr);

#endif /* __XFS_EXCHRANGE_H__ */
