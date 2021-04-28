/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_RT_RESV_H__
#define __XFS_RT_RESV_H__

bool xfs_rt_resv_critical(struct xfs_mount *mp, struct xfs_inode *ip);
void xfs_rt_resv_free(struct xfs_mount *mp);
int xfs_rt_resv_init(struct xfs_mount *mp);

struct xfs_alloc_arg;

void xfs_rt_resv_alloc_extent(struct xfs_inode *ip, struct xfs_alloc_arg *args);
void xfs_rt_resv_free_extent(struct xfs_inode *ip, struct xfs_trans *tp,
		xfs_filblks_t len);

#endif /* __XFS_RT_RESV_H__ */
