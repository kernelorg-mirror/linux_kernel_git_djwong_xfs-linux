/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SWAPRANGE_H__
#define __XFS_SWAPRANGE_H__

struct xfs_swapext_req;
struct xfs_swapext_res;

int xfs_swap_range_estimate(const struct xfs_swapext_req *req,
		struct xfs_swapext_res *res);

#endif /* __XFS_SWAPRANGE_H__ */
