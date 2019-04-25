// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_IWALK_H__
#define __XFS_IWALK_H__

/* Walk all inodes in the filesystem starting from @startino. */
typedef int (*xfs_iwalk_fn)(struct xfs_mount *mp, xfs_ino_t ino, void *data);
#define XFS_IWALK_ABORT	(1)
int xfs_iwalk(struct xfs_mount *mp, xfs_ino_t startino, xfs_iwalk_fn iwalk_fn,
		void *data);
int xfs_iwalk_threaded(struct xfs_mount *mp, xfs_ino_t startino,
		xfs_iwalk_fn iwalk_fn, void *data);

#endif /* __XFS_IWALK_H__ */
