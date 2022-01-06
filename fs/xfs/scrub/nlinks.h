/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_NLINKS_H__
#define __XFS_SCRUB_NLINKS_H__

/* Live link count control structure. */
struct xchk_nlink_ctrs {
	struct xfs_scrub	*sc;

	/* Shadow link count data and its mutex. */
	struct xfarray		*nlinks;
	struct mutex		lock;

	/*
	 * The collection step uses a separate iscan context from the compare
	 * step because the collection iscan coordinates live updates to the
	 * observation data while this scanner is running.  The compare iscan
	 * is secondary and can be reinitialized as needed.
	 */
	struct xchk_iscan	collect_iscan;
	struct xchk_iscan	compare_iscan;

	/*
	 * Hook into bumplink/droplink so that we can receive live updates
	 * from other writer threads.
	 */
	struct notifier_block	nlink_delta_hook;
};

struct xchk_nlink {
	/* Links from a parent directory to this inode. */
	xfs_nlink_t		parent;

	/* Links from children of this inode (e.g. dot and dotdot). */
	xfs_nlink_t		child;

	/* Record state flags */
	unsigned int		flags;
};

/* This data item was seen by the check-time compare function. */
#define XCHK_NLINK_COMPARE_SCANNED	(1U << 0)

/* Compute total link count, using large enough variables to detect overflow. */
static inline uint64_t
xchk_nlink_total(const struct xchk_nlink *live)
{
	uint64_t	ret = live->parent;

	return ret + live->child;
}

#endif /* __XFS_SCRUB_NLINKS_H__ */
