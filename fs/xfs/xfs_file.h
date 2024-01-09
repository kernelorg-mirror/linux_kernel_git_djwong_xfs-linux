// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __XFS_FILE_H__
#define __XFS_FILE_H__

extern const struct file_operations xfs_file_operations;
extern const struct file_operations xfs_dir_file_operations;

bool xfs_is_falloc_aligned(struct xfs_inode *ip, loff_t pos,
		long long int len);

bool xfs_truncate_needs_cow_around(struct xfs_inode *ip, loff_t pos);
int xfs_file_unshare_at(struct xfs_inode *ip, loff_t pos);

#endif /* __XFS_FILE_H__ */
