/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_NLINKS_H__
#define __XFS_SCRUB_NLINKS_H__

/* Live link count control structure. */
struct xchk_nlinks {
	struct xfs_scrub	*sc;

	/* Shadow link count data. */
	struct xfbma		*nlinks;

	/* Last inode scanned by the inode walk. */
	xfs_ino_t		last_ino;

	/* Hooks into bumplink/droplink code. */
	struct notifier_block	mod_hook;

	/* Lock for the data used to capture live nlink updates. */
	struct mutex		lock;

	/* Something failed during live tracking. */
	bool			hook_dead;
};

int xchk_nlinks_get_shadow_count(struct xchk_nlinks *xnc, xfs_ino_t ino,
		xfs_nlink_t *nlinks);

#endif /* __XFS_SCRUB_NLINKS_H__ */
