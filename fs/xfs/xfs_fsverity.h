/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 Red Hat, Inc.
 */
#ifndef __XFS_FSVERITY_H__
#define __XFS_FSVERITY_H__

#ifdef CONFIG_FS_VERITY
struct xfs_merkle_bkey {
	/* inumber of the file */
	xfs_ino_t		ino;

	/* the position of the block in the Merkle tree (in bytes) */
	u64			pos;
};

void xfs_fsverity_destroy_inode(struct xfs_inode *ip);

int xfs_fsverity_mount(struct xfs_mount *mp);
void xfs_fsverity_unmount(struct xfs_mount *mp);
int xfs_fsverity_growfs(struct xfs_mount *mp, xfs_agnumber_t old_agcount,
		xfs_agnumber_t new_agcount);

extern const struct fsverity_operations xfs_fsverity_ops;
#else
# define xfs_fsverity_destroy_inode(ip)		((void)0)
# define xfs_fsverity_mount(mp)			(0)
# define xfs_fsverity_unmount(mp)		((void)0)
# define xfs_fsverity_growfs(mp, o, n)		(0)
#endif	/* CONFIG_FS_VERITY */

#endif	/* __XFS_FSVERITY_H__ */
