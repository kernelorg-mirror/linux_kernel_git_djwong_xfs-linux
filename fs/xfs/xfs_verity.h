/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 Red Hat, Inc.
 */
#ifndef __XFS_VERITY_H__
#define __XFS_VERITY_H__

#ifdef CONFIG_FS_VERITY
void xfs_verity_cache_init(struct xfs_inode *ip);
void xfs_verity_cache_drop(struct xfs_inode *ip);
void xfs_verity_cache_destroy(struct xfs_inode *ip);

int xfs_verity_register_shrinker(struct xfs_mount *mp);
void xfs_verity_unregister_shrinker(struct xfs_mount *mp);

struct xfs_icwalk;
int xfs_verity_scan_inode(struct xfs_inode *ip, struct xfs_icwalk *icw);

extern const struct fsverity_operations xfs_verity_ops;
#else
# define xfs_verity_cache_init(ip)		((void)0)
# define xfs_verity_cache_drop(ip)		((void)0)
# define xfs_verity_cache_destroy(ip)		((void)0)
# define xfs_verity_register_shrinker(mp)	(0)
# define xfs_verity_unregister_shrinker(mp)	((void)0)
# define xfs_verity_scan_inode(ip, icw)		(0)
#endif	/* CONFIG_FS_VERITY */

#endif	/* __XFS_VERITY_H__ */
