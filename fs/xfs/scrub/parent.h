/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SCRUB_PARENT_H__
#define __XFS_SCRUB_PARENT_H__

int xchk_parent_count_parent_dentries(struct xfs_scrub *sc,
		struct xfs_inode *parent, xfs_nlink_t *nlink);

typedef int (*xrep_parents_iter_fn)(struct xfs_inode *dp, struct xfs_name *name,
		unsigned int dtype, void *data);
int xrep_scan_for_parents(struct xfs_scrub *sc, xfs_ino_t target_ino,
		xrep_parents_iter_fn fn, void *data);
bool xrep_parent_acceptable(struct xfs_scrub *sc, xfs_ino_t ino);

#endif /* __XFS_SCRUB_PARENT_H__ */
