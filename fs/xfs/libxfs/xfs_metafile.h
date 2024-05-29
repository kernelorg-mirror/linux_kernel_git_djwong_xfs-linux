/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_METAFILE_H__
#define __XFS_METAFILE_H__

void xfs_metafile_set_iflag(struct xfs_trans *tp, struct xfs_inode *ip);
void xfs_metafile_clear_iflag(struct xfs_trans *tp, struct xfs_inode *ip);

/* Code specific to kernel/userspace; must be provided externally. */

int xfs_metafile_iget(struct xfs_trans *tp, xfs_ino_t ino, umode_t mode,
		struct xfs_inode **ipp);

#endif /* __XFS_METAFILE_H__ */
