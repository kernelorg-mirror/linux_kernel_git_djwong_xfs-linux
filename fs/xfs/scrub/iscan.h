/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_ISCAN_H__
#define __XFS_SCRUB_ISCAN_H__

struct xchk_iscan {
	/* Lock to protect the scan cursor. */
	struct mutex		lock;

	/* This is the inode that is being scanned. */
	xfs_ino_t		cursor_ino;

	/*
	 * This is the last inode that we've successfully scanned, either
	 * because the caller scanned it, or we moved the cursor past an empty
	 * part of the inode address space.  Scan callers should only use the
	 * xchk_iscan_visit function to modify this.
	 */
	xfs_ino_t		__visited_ino;

	/* Number of times to try iget calls for any inode. */
	unsigned int		iget_tries;

	/* Number of tries remaining for iget of cursor_ino.  Do not modify. */
	unsigned int		__cursor_tries;
};

void xchk_iscan_start(struct xchk_iscan *iscan, unsigned int iget_tries);
void xchk_iscan_finish(struct xchk_iscan *iscan);

int xchk_iscan_advance(struct xfs_scrub *sc, struct xchk_iscan *iscan);
int xchk_iscan_iget(struct xfs_scrub *sc, struct xchk_iscan *iscan,
		struct xfs_inode **ipp);

static inline void
xchk_iscan_lock(struct xchk_iscan *iscan)
{
	mutex_lock(&iscan->lock);
}

static inline void
xchk_iscan_unlock(struct xchk_iscan *iscan)
{
	mutex_unlock(&iscan->lock);
}

void __xchk_iscan_visit(struct xchk_iscan *iscan, struct xfs_inode *ip);

static inline void
xchk_iscan_visit(
	struct xchk_iscan	*iscan,
	struct xfs_inode	*ip)
{
	xchk_iscan_lock(iscan);
	__xchk_iscan_visit(iscan, ip);
	xchk_iscan_unlock(iscan);
}

#endif /* __XFS_SCRUB_ISCAN_H__ */
