/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_NOTIFY_FAILURE_H__
#define __XFS_NOTIFY_FAILURE_H__

extern const struct dax_holder_operations xfs_dax_holder_operations;

enum xfs_failed_device {
	XFS_FAILED_DATADEV,
	XFS_FAILED_LOGDEV,
	XFS_FAILED_RTDEV,
};

struct xfs_media_error_params {
	xfs_daddr_t			daddr;
	uint64_t			bbcount;
	enum xfs_failed_device		fdev;
	bool				pre_remove;
};

#endif /* __XFS_NOTIFY_FAILURE_H__ */
