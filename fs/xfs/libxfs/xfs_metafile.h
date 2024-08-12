/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_METAFILE_H__
#define __XFS_METAFILE_H__

void xfs_metafile_set_iflag(struct xfs_trans *tp, struct xfs_inode *ip,
		enum xfs_metafile_type metafile_type);
void xfs_metafile_clear_iflag(struct xfs_trans *tp, struct xfs_inode *ip);

/* Space reservations for metadata inodes. */
struct xfs_alloc_arg;

bool xfs_metafile_resv_critical(struct xfs_inode *ip);
void xfs_metafile_resv_alloc_space(struct xfs_inode *ip,
		struct xfs_alloc_arg *args);
void xfs_metafile_resv_free_space(struct xfs_inode *ip, struct xfs_trans *tp,
		xfs_filblks_t len);
void xfs_metafile_resv_free(struct xfs_inode *ip);
int xfs_metafile_resv_init(struct xfs_inode *ip, xfs_filblks_t ask);

/* Code specific to kernel/userspace; must be provided externally. */

int xfs_metafile_iget(struct xfs_trans *tp, xfs_ino_t ino,
		enum xfs_metafile_type metafile_type, struct xfs_inode **ipp);

#endif /* __XFS_METAFILE_H__ */
