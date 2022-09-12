// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Oracle, Inc.
 * All rights reserved.
 */
#ifndef	__XFS_PARENT_UTILS_H__
#define	__XFS_PARENT_UTILS_H__

int xfs_attr_get_parent_pointer(struct xfs_inode *ip,
				struct xfs_pptr_info *ppi);
#endif	/* __XFS_PARENT_UTILS_H__ */
