/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 Red Hat, Inc.
 */
#ifndef __XFS_FSVERITY_H__
#define __XFS_FSVERITY_H__

#ifdef CONFIG_FS_VERITY
void xfs_fsverity_cache_init(struct xfs_inode *ip);
void xfs_fsverity_destroy_inode(struct xfs_inode *ip);
void xfs_fsverity_cache_destroy(struct xfs_inode *ip);

extern const struct fsverity_operations xfs_fsverity_ops;
#else
# define xfs_fsverity_cache_init(ip)		((void)0)
# define xfs_fsverity_destroy_inode(ip)		((void)0)
# define xfs_fsverity_cache_destroy(ip)		((void)0)
#endif	/* CONFIG_FS_VERITY */

#endif	/* __XFS_FSVERITY_H__ */
