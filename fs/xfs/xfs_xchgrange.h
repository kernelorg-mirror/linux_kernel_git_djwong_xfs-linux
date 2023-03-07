/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2020-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_XCHGRANGE_H__
#define __XFS_XCHGRANGE_H__

/* Prepare generic VFS data structures for file exchanges */

int xfs_exch_range_prep(struct file *file1, struct file *file2,
		struct xfs_exch_range *fxr, unsigned int blocksize);
int xfs_exch_range_finish(struct file *file1, struct file *file2);

int xfs_exch_range(struct file *file1, struct file *file2,
		struct xfs_exch_range *fxr);

/* XFS-specific parts of file exchanges */

struct xfs_swapext_req;

void xfs_xchg_range_ilock(struct xfs_trans *tp, struct xfs_inode *ip1,
		struct xfs_inode *ip2);
void xfs_xchg_range_iunlock(struct xfs_inode *ip1, struct xfs_inode *ip2);

int xfs_xchg_range_estimate(struct xfs_swapext_req *req);

#endif /* __XFS_XCHGRANGE_H__ */
