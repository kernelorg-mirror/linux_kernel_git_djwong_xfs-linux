/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_PARENT_H__
#define __XFS_SCRUB_PARENT_H__

int xchk_parent_lock_two_dirs(struct xfs_scrub *sc, struct xfs_inode *dp);

int xrep_parent_confirm(struct xfs_scrub *sc, xfs_ino_t *parent_ino);
int xrep_parent_scan(struct xfs_scrub *sc, xfs_ino_t *parent_ino);

#endif /* __XFS_SCRUB_PARENT_H__ */
