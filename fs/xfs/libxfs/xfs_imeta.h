/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_IMETA_H__
#define __XFS_IMETA_H__

/* How deep can we nest metadata dirs? */
#define XFS_IMETA_MAX_DEPTH	64

/* Key for looking up metadata inodes. */
struct xfs_imeta_path {
	/* Array of string pointers. */
	const char		**im_path;

	/* Set bits correspond to components of im_path needing to be freed */
	unsigned long long	im_dynamicmask;

	/* Number of strings in path.  0 here means the metadir root. */
	uint8_t			im_depth;

	/* Expected file type. */
	uint8_t			im_ftype;
};

/* Cleanup widget for metadata inode creation and deletion. */
struct xfs_imeta_update {
	struct xfs_mount	*mp;
	struct xfs_trans	*tp;

	/* Path to metadata file */
	const struct xfs_imeta_path *path;

	/* Parent pointer update context */
	struct xfs_parent_args	*ppargs;

	/* Parent directory */
	struct xfs_inode	*dp;

	/* Metadata inode */
	struct xfs_inode	*ip;

	unsigned int		dp_locked:1;
	unsigned int		ip_locked:1;
};

/* Grab the last path component, mostly for tracing. */
static inline const char *
xfs_imeta_lastpath(
	const struct xfs_imeta_update	*upd)
{
	if (upd->path && upd->path->im_path && upd->path->im_depth > 0)
		return upd->path->im_path[upd->path->im_depth - 1];
	return "?";
}

int xfs_imeta_lookup(struct xfs_trans *tp, const struct xfs_imeta_path *path,
		xfs_ino_t *ino);
int xfs_imeta_iget_parent(struct xfs_trans *tp,
		const struct xfs_imeta_path *path, struct xfs_inode **dpp);

int xfs_imeta_create_file_path(struct xfs_mount *mp,
		unsigned int nr_components, struct xfs_imeta_path **pathp);
void xfs_imeta_free_path(const struct xfs_imeta_path *path);

void xfs_imeta_set_iflag(struct xfs_trans *tp, struct xfs_inode *ip);
void xfs_imeta_clear_iflag(struct xfs_trans *tp, struct xfs_inode *ip);

int xfs_imeta_ensure_dirpath(struct xfs_mount *mp,
		const struct xfs_imeta_path *path);

int xfs_imeta_start_create(struct xfs_mount *mp,
		const struct xfs_imeta_path *path,
		struct xfs_imeta_update *upd);
int xfs_imeta_create(struct xfs_imeta_update *upd, umode_t mode);

int xfs_imeta_start_link(struct xfs_mount *mp,
		const struct xfs_imeta_path *path,
		struct xfs_inode *ip, struct xfs_imeta_update *upd);
int xfs_imeta_link(struct xfs_imeta_update *upd);

int xfs_imeta_commit(struct xfs_imeta_update *upd);
void xfs_imeta_cancel(struct xfs_imeta_update *upd, int error);

/* Space reservations for metadata inodes. */
struct xfs_alloc_arg;

bool xfs_imeta_resv_critical(struct xfs_inode *ip);
void xfs_imeta_resv_alloc_extent(struct xfs_inode *ip,
		struct xfs_alloc_arg *args);
void xfs_imeta_resv_free_extent(struct xfs_inode *ip, struct xfs_trans *tp,
		xfs_filblks_t len);
void xfs_imeta_resv_free_inode(struct xfs_inode *ip);
int xfs_imeta_resv_init_inode(struct xfs_inode *ip, xfs_filblks_t ask);

/* Code specific to kernel/userspace; must be provided externally. */

int xfs_imeta_iget(struct xfs_trans *tp, xfs_ino_t ino, umode_t mode,
		struct xfs_inode **ipp);

#endif /* __XFS_IMETA_H__ */
