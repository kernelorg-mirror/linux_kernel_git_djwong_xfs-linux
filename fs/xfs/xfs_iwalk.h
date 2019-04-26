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
		xfs_iwalk_fn iwalk_fn, bool poll, void *data);

/* Walk all inode btree records in the filesystem starting from @startino. */
typedef int (*xfs_inobt_walk_fn)(struct xfs_mount *mp, xfs_agnumber_t agno,
		const struct xfs_inobt_rec_incore *irec, void *data);
#define XFS_INOBT_WALK_ABORT	(XFS_IWALK_ABORT)
int xfs_inobt_walk(struct xfs_mount *mp, xfs_ino_t startino,
		xfs_inobt_walk_fn inobt_walk_fn, void *data);

#endif /* __XFS_IWALK_H__ */
