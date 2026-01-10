/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_NOTIFY_FAILURE_H__
#define __XFS_NOTIFY_FAILURE_H__

extern const struct dax_holder_operations xfs_dax_holder_operations;

struct xfs_verify_media;
int xfs_ioc_verify_media(struct file *file,
		struct xfs_verify_media __user *arg);

#endif /* __XFS_NOTIFY_FAILURE_H__ */
