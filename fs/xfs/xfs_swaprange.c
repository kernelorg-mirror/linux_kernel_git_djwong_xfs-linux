// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_swapext.h"
#include "xfs_swaprange.h"

/*
 * Estimate the resource requirements to swap ranges between the two files.
 * The caller is required to hold the IOLOCK and the MMAPLOCK and to have
 * flushed both inodes' pagecache and active directios.
 */
int
xfs_swap_range_estimate(
	const struct xfs_swapext_req	*req,
	struct xfs_swapext_res		*res)
{
	int				error;

	if (req->ip1 != req->ip2)
		xfs_lock_two_inodes(req->ip1, XFS_ILOCK_EXCL,
				    req->ip2, XFS_ILOCK_EXCL);
	else
		xfs_ilock(req->ip1, XFS_ILOCK_EXCL);
	error = xfs_swapext_estimate(req, res);
	xfs_iunlock(req->ip1, XFS_ILOCK_EXCL);
	if (req->ip2 != req->ip1)
		xfs_iunlock(req->ip2, XFS_ILOCK_EXCL);
	return error;
}
