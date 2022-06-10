/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_XCHGRANGE_H__
#define __XFS_XCHGRANGE_H__

struct xfs_swapext_req;

void xfs_xchg_range_ilock(struct xfs_trans *tp, struct xfs_inode *ip1,
		struct xfs_inode *ip2);
void xfs_xchg_range_iunlock(struct xfs_inode *ip1, struct xfs_inode *ip2);

int xfs_xchg_range_estimate(struct xfs_swapext_req *req);

int xfs_xchg_range_grab_log_assist(struct xfs_mount *mp, bool force,
		bool *use_logging);
void xfs_xchg_range_rele_log_assist(struct xfs_mount *mp, bool need_rele);

/* Caller has permission to use log intent items for the exchange operation. */
#define XFS_XCHG_RANGE_LOGGED		(1U << 0)

/* Update ip1's change and mod time. */
#define XFS_XCHG_RANGE_UPD_CMTIME1	(1U << 1)

/* Update ip2's change and mod time. */
#define XFS_XCHG_RANGE_UPD_CMTIME2	(1U << 2)

#define XCHG_RANGE_FLAGS_STRS \
	{ XFS_XCHG_RANGE_LOGGED,		"LOGGED" }, \
	{ XFS_XCHG_RANGE_UPD_CMTIME1,		"UPD_CMTIME1" }, \
	{ XFS_XCHG_RANGE_UPD_CMTIME2,		"UPD_CMTIME2" }

int xfs_xchg_range(struct xfs_inode *ip1, struct xfs_inode *ip2,
		const struct file_xchg_range *fxr, unsigned int xchg_flags);
int xfs_xchg_range_prep(struct file *file1, struct file *file2,
		struct file_xchg_range *fxr);

#endif /* __XFS_XCHGRANGE_H__ */
