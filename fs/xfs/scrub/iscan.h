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
	 * xchk_iscan_mark function to modify this.
	 */
	xfs_ino_t		marked_ino;
};

void xchk_iscan_start(struct xchk_iscan *iscan);
void xchk_iscan_finish(struct xchk_iscan *iscan);

int xchk_iscan_advance(struct xchk_iscan *iscan, struct xfs_trans *tp);

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

/*
 * If the caller cannot get the cursor inode, set us up so that the next
 * advance call will re-query the inobt at the same location.
 */
static inline void
xchk_iscan_retry(struct xchk_iscan *iscan)
{
	ASSERT(iscan->cursor_ino == iscan->marked_ino + 1);

	iscan->cursor_ino--;
}

/* Mark this inode as having been scanned. */
static inline void
xchk_iscan_mark(struct xchk_iscan *iscan, struct xfs_inode *ip)
{
	xchk_iscan_lock(iscan);
	iscan->marked_ino = ip->i_ino;
	xchk_iscan_unlock(iscan);
}

static inline void
xchk_iscan_mark_locked(struct xchk_iscan *iscan, struct xfs_inode *ip)
{
	iscan->marked_ino = ip->i_ino;
}

/* Decide if this inode was previously scanned. */
static inline bool
xchk_iscan_marked(struct xchk_iscan *iscan, struct xfs_inode *ip)
{
	return iscan->marked_ino >= ip->i_ino;
}

#endif /* __XFS_SCRUB_ISCAN_H__ */
