// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_inode.h"
#include "xfs_btree.h"
#include "xfs_ialloc.h"
#include "xfs_ialloc_btree.h"
#include "xfs_ag.h"
#include "xfs_error.h"
#include "xfs_bit.h"
#include "scrub/iscan.h"

/*
 * Live Inode Scan
 * ===============
 *
 * Live inode scans walk every inode in a live filesystem.  This is more or
 * less like a regular iwalk, except that when we're advancing the scan cursor,
 * we must ensure that inodes cannot be added or deleted anywhere between the
 * old cursor value and the new cursor value.  If we're advancing the cursor
 * by one inode, the caller must hold that inode; if we're finding the next
 * inode to scan, we must grab the AGI and hold it until we've updated the
 * scan cursor.
 *
 * Callers are expected to use this code to scan all files in the filesystem to
 * construct a new metadata index of some kind.  The scan races against other
 * live updates, which means there must be a provision to update the new index
 * when updates are made to inodes that already been scanned.  The iscan lock
 * can be used in live update hook code to stop the scan and protect this data
 * structure.
 */

/*
 * Set the bits in @irec's free mask that correspond to the inodes before
 * @agino so that we skip them.  This is how we restart an inode walk that was
 * interrupted in the middle of an inode record.
 */
STATIC void
xchk_iscan_adjust_start(
	xfs_agino_t			agino,	/* starting inode of chunk */
	struct xfs_inobt_rec_incore	*irec)	/* btree record */
{
	int				idx;	/* index into inode chunk */

	idx = agino - irec->ir_startino;

	irec->ir_free |= xfs_inobt_maskn(0, idx);
	irec->ir_freecount = hweight64(irec->ir_free);
}

/*
 * Set *cursor to the next allocated inode after whatever it's set to now.
 * If there are no more inodes in this AG, cursor is set to NULLAGINO.
 */
int
xchk_iscan_find_next(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	struct xfs_buf		*agi_bp,
	struct xfs_perag	*pag,
	xfs_agino_t		*cursor)
{
	struct xfs_inobt_rec_incore	rec;
	struct xfs_btree_cur	*cur;
	xfs_agnumber_t		agno = pag->pag_agno;
	xfs_agino_t		lastino = NULLAGINO;
	xfs_agino_t		first, last;
	xfs_agino_t		agino = *cursor;
	int			has_rec;
	int			error;

	/* If the cursor is beyond the end of this AG, move to the next one. */
	xfs_agino_range(mp, agno, &first, &last);
	if (agino > last) {
		*cursor = NULLAGINO;
		return 0;
	}

	/*
	 * Look up the inode chunk for the current cursor position.  If there
	 * is no chunk here, we want the next one.
	 */
	cur = xfs_inobt_init_cursor(mp, tp, agi_bp, pag, XFS_BTNUM_INO);
	error = xfs_inobt_lookup(cur, agino, XFS_LOOKUP_LE, &has_rec);
	if (!error && !has_rec)
		error = xfs_btree_increment(cur, 0, &has_rec);
	for (; !error; error = xfs_btree_increment(cur, 0, &has_rec)) {
		/*
		 * If we've run out of inobt records in this AG, move the
		 * cursor on to the next AG and exit.  The caller can try
		 * again with the next AG.
		 */
		if (!has_rec) {
			*cursor = NULLAGINO;
			break;
		}

		error = xfs_inobt_get_rec(cur, &rec, &has_rec);
		if (error)
			break;
		if (!has_rec) {
			error = -EFSCORRUPTED;
			break;
		}

		/* Make sure that we always move forward. */
		if (lastino != NULLAGINO &&
		    XFS_IS_CORRUPT(mp, lastino >= rec.ir_startino)) {
			error = -EFSCORRUPTED;
			break;
		}
		lastino = rec.ir_startino + XFS_INODES_PER_CHUNK - 1;

		/*
		 * If this record only covers inodes that come before the
		 * cursor, advance to the next record.
		 */
		if (rec.ir_startino + XFS_INODES_PER_CHUNK <= agino)
			continue;

		/*
		 * If the incoming lookup put us in the middle of an inobt
		 * record, mark it and the previous inodes "free" so that the
		 * search for allocated inodes will start at the cursor.  Use
		 * funny math to avoid overflowing the bit shift.
		 */
		if (agino >= rec.ir_startino)
			xchk_iscan_adjust_start(agino + 1, &rec);

		/*
		 * If there are allocated inodes in this chunk, find them,
		 * and update the cursor.
		 */
		if (rec.ir_freecount < XFS_INODES_PER_CHUNK) {
			int	next = xfs_lowbit64(~rec.ir_free);

			*cursor = rec.ir_startino + next;
			break;
		}
	}

	xfs_btree_del_cursor(cur, error);
	return error;
}

/*
 * Prepare to return agno/agino to the iscan caller by moving the lastino
 * cursor to the previous inode.  Do this while we still hold the AGI so that
 * no other threads can create or delete inodes in this AG.
 */
static inline void
xchk_iscan_move_cursor(
	struct xchk_iscan	*iscan,
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	xfs_agino_t		agino)
{
	mutex_lock(&iscan->lock);
	iscan->cursor_ino = XFS_AGINO_TO_INO(mp, agno, agino);
	iscan->marked_ino = iscan->cursor_ino - 1;
	mutex_unlock(&iscan->lock);
}

/*
 * Advance ino to the next inode that the inobt thinks is allocated, being
 * careful to jump to the next AG and to skip quota inodes.  Advancing ino
 * effectively means that we've pushed the quotacheck scan forward, so set the
 * quotacheck cursor to (ino - 1) so that our shadow dquot tracking will track
 * inode allocations in that range once we release the AGI buffer.
 */
int
xchk_iscan_advance(
	struct xchk_iscan	*iscan,
	struct xfs_trans	*tp)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_buf		*agi_bp;
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno;
	xfs_agino_t		agino;
	int			error;

	ASSERT(iscan->cursor_ino >= iscan->marked_ino);

next_ag:
	agno = XFS_INO_TO_AGNO(mp, iscan->cursor_ino);
	if (agno >= mp->m_sb.sb_agcount) {
		xchk_iscan_move_cursor(iscan, mp, agno, 0);
		iscan->cursor_ino = NULLFSINO;
		return 0;
	}
	agino = XFS_INO_TO_AGINO(mp, iscan->cursor_ino);

	pag = xfs_perag_get(mp, agno);
	error = xfs_ialloc_read_agi(mp, tp, agno, &agi_bp);
	if (error)
		goto out_pag;

	error = xchk_iscan_find_next(mp, tp, agi_bp, pag, &agino);
	if (error)
		goto out_buf;
	if (agino == NULLAGINO) {
		xchk_iscan_move_cursor(iscan, mp, agno + 1, 0);
		xfs_trans_brelse(tp, agi_bp);
		xfs_perag_put(pag);
		goto next_ag;
	}

	xchk_iscan_move_cursor(iscan, mp, agno, agino);
out_buf:
	xfs_trans_brelse(tp, agi_bp);
out_pag:
	xfs_perag_put(pag);
	return error;
}

void
xchk_iscan_finish(
	struct xchk_iscan	*iscan)
{
	mutex_destroy(&iscan->lock);
	iscan->cursor_ino = NULLFSINO;
	iscan->marked_ino = NULLFSINO;
}

void
xchk_iscan_start(
	struct xchk_iscan	*iscan)
{
	iscan->marked_ino = 0;
	iscan->cursor_ino = 0;
	mutex_init(&iscan->lock);
}
