/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SCRUB_PARENT_H__
#define __XFS_SCRUB_PARENT_H__

int xrep_dir_parent_find(struct xfs_scrub *sc, xfs_ino_t *parent_ino);

#endif /* __XFS_SCRUB_PARENT_H__ */
