/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_IMETA_H__
#define __XFS_IMETA_H__

/* Cleanup widget for metadata inode creation and deletion. */
struct xfs_imeta_update {
	struct xfs_mount	*mp;
	struct xfs_trans	*tp;

	/* Path to metadata file */
	const char		*path;

	/* Parent pointer update context */
	struct xfs_parent_args	*ppargs;

	/* Parent directory */
	struct xfs_inode	*dp;

	/* Metadata inode */
	struct xfs_inode	*ip;

	unsigned int		dp_locked:1;
	unsigned int		ip_locked:1;
};

int xfs_imeta_load(struct xfs_trans *tp, struct xfs_inode *dp,
		const char *path, umode_t mode, struct xfs_inode **ipp);

void xfs_imeta_set_iflag(struct xfs_trans *tp, struct xfs_inode *ip);
void xfs_imeta_clear_iflag(struct xfs_trans *tp, struct xfs_inode *ip);

int xfs_imeta_start_create(struct xfs_inode *dp, const char *path,
		struct xfs_imeta_update *upd);
int xfs_imeta_create(struct xfs_imeta_update *upd, umode_t mode);

int xfs_imeta_start_link(struct xfs_inode *dp, const char *path,
		struct xfs_inode *ip, struct xfs_imeta_update *upd);
int xfs_imeta_link(struct xfs_imeta_update *upd);

int xfs_imeta_commit(struct xfs_imeta_update *upd);
void xfs_imeta_cancel(struct xfs_imeta_update *upd, int error);

int xfs_imeta_mkdir(struct xfs_inode *dp, const char *path,
		struct xfs_inode **ipp);

/* Code specific to kernel/userspace; must be provided externally. */

int xfs_imeta_iget(struct xfs_trans *tp, xfs_ino_t ino, umode_t mode,
		struct xfs_inode **ipp);

#endif /* __XFS_IMETA_H__ */
