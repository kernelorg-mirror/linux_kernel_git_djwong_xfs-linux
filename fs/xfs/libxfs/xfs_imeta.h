/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_IMETA_H__
#define __XFS_IMETA_H__

/* Code specific to kernel/userspace; must be provided externally. */

int xfs_imeta_iget(struct xfs_trans *tp, xfs_ino_t ino, umode_t mode,
		struct xfs_inode **ipp);

#endif /* __XFS_IMETA_H__ */
