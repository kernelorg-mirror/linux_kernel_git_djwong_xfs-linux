/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SWAPRANGE_H__
#define __XFS_SWAPRANGE_H__

struct xfs_swapext_req;
struct xfs_swapext_res;

void xfs_swap_range_ilock(struct xfs_trans *tp, struct xfs_inode *ip1,
		struct xfs_inode *ip2);
void xfs_swap_range_iunlock(struct xfs_inode *ip1, struct xfs_inode *ip2);

int xfs_swap_range_estimate(const struct xfs_swapext_req *req,
		struct xfs_swapext_res *res);
int xfs_swap_range_prep(struct file *file1, struct file *file2,
		struct file_swap_range *fsr);

/* Update ip1's change and mod time. */
#define XFS_SWAP_RANGE_UPD_CMTIME1	(1 << 0)

/* Update ip2's change and mod time. */
#define XFS_SWAP_RANGE_UPD_CMTIME2	(1 << 1)

int xfs_swap_range(struct xfs_inode *ip1, struct xfs_inode *ip2,
		const struct file_swap_range *fsr, unsigned int private_flags);

int xfs_add_atomic_swap(struct xfs_mount *mp);

#endif /* __XFS_SWAPRANGE_H__ */
