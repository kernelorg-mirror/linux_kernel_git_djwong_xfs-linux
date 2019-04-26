// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_btree.h"
#include "xfs_ialloc.h"
#include "xfs_ialloc_btree.h"
#include "xfs_itable.h"
#include "xfs_error.h"
#include "xfs_trace.h"
#include "xfs_icache.h"
#include "xfs_health.h"
#include "xfs_trans.h"
#include "xfs_iwalk.h"

/*
 * Walking All the Inodes in the Filesystem
 * ========================================
 * Starting at some @startino, call a walk function on every allocated inode in
 * the system.  The walk function is called with the relevant inode number and
 * a pointer to caller-provided data.  The walk function can return the usual
 * negative error code, 0, or XFS_IWALK_ABORT to stop the iteration.  This
 * return value is returned to the caller.
 *
 * Internally, we allow the walk function to do anything, which means that we
 * cannot maintain the inobt cursor or our lock on the AGI buffer.  We
 * therefore build up a batch of inobt records in kernel memory and only call
 * the walk function when our memory buffer is full.
 */

struct xfs_iwalk_ag {
	struct xfs_mount		*mp;

	/* Where do we start the traversal? */
	xfs_ino_t			startino;

	/* Array of inobt records we cache. */
	struct xfs_inobt_rec_incore	*recs;
	unsigned int			sz_recs;
	unsigned int			nr_recs;

	/* Inode walk function and data pointer. */
	xfs_iwalk_fn			iwalk_fn;
	void				*data;
};

/*
 * Loop over all clusters in a chunk for a given incore inode allocation btree
 * record.  Do a readahead if there are any allocated inodes in that cluster.
 */
STATIC void
xfs_iwalk_ichunk_ra(
	struct xfs_mount		*mp,
	xfs_agnumber_t			agno,
	struct xfs_inobt_rec_incore	*irec)
{
	struct xfs_ino_geometry		*igeo = &mp->m_ino_geo;
	xfs_agblock_t			agbno;
	struct blk_plug			plug;
	int				i;	/* inode chunk index */

	agbno = XFS_AGINO_TO_AGBNO(mp, irec->ir_startino);

	blk_start_plug(&plug);
	for (i = 0;
	     i < XFS_INODES_PER_CHUNK;
	     i += igeo->ig_inodes_per_cluster,
			agbno += igeo->ig_blocks_per_cluster) {
		if (xfs_inobt_maskn(i, igeo->ig_inodes_per_cluster) &
		    ~irec->ir_free) {
			xfs_btree_reada_bufs(mp, agno, agbno,
					igeo->ig_blocks_per_cluster,
					&xfs_inode_buf_ops);
		}
	}
	blk_finish_plug(&plug);
}

/*
 * Lookup the inode chunk that the given @agino lives in and then get the
 * record if we found the chunk.  Set the bits in @irec's free mask that
 * correspond to the inodes before @agino so that we skip them.  This is how we
 * restart an inode walk that was interrupted in the middle of an inode record.
 */
STATIC int
xfs_iwalk_grab_ichunk(
	struct xfs_btree_cur		*cur,	/* btree cursor */
	xfs_agino_t			agino,	/* starting inode of chunk */
	int				*icount,/* return # of inodes grabbed */
	struct xfs_inobt_rec_incore	*irec)	/* btree record */
{
	int				idx;	/* index into inode chunk */
	int				stat;
	int				i;
	int				error = 0;

	/* Lookup the inode chunk that this inode lives in */
	error = xfs_inobt_lookup(cur, agino, XFS_LOOKUP_LE, &stat);
	if (error)
		return error;
	if (!stat) {
		*icount = 0;
		return error;
	}

	/* Get the record, should always work */
	error = xfs_inobt_get_rec(cur, irec, &stat);
	if (error)
		return error;
	XFS_WANT_CORRUPTED_RETURN(cur->bc_mp, stat == 1);

	/* Check if the record contains the inode in request */
	if (irec->ir_startino + XFS_INODES_PER_CHUNK <= agino) {
		*icount = 0;
		return 0;
	}

	idx = agino - irec->ir_startino;

	/*
	 * We got a right chunk with some left inodes allocated at it.  Grab
	 * the chunk record.  Mark all the uninteresting inodes free because
	 * they're before our start point.
	 */
	for (i = 0; i < idx; i++) {
		if (XFS_INOBT_MASK(i) & ~irec->ir_free)
			irec->ir_freecount++;
	}

	irec->ir_free |= xfs_inobt_maskn(0, idx);
	*icount = irec->ir_count - irec->ir_freecount;
	return 0;
}

/* Allocate memory for a walk. */
STATIC int
xfs_iwalk_alloc(
	struct xfs_iwalk_ag	*iwag)
{
	size_t			size;

	ASSERT(iwag->recs == NULL);
	iwag->nr_recs = 0;

	/* First we try for four pages worth of buffer. */
	size = PAGE_SIZE * 4;
	iwag->recs = kmem_alloc_large(size, KM_SLEEP | KM_MAYFAIL);
	if (iwag->recs) {
		iwag->sz_recs = size / sizeof(struct xfs_inobt_rec_incore);
		return 0;
	}

	/* If not, try for one page. */
	size = PAGE_SIZE;
	iwag->recs = kmem_alloc(size, KM_SLEEP);
	if (iwag->recs) {
		iwag->sz_recs = size / sizeof(struct xfs_inobt_rec_incore);
		return 0;
	}

	return -ENOMEM;
}

/* Free memory we allocated for a walk. */
STATIC void
xfs_iwalk_free(
	struct xfs_iwalk_ag	*iwag)
{
	ASSERT(iwag->recs != NULL);
	kmem_free(iwag->recs);
	iwag->sz_recs = 0;
}

/* For each inuse inode in each cached inobt record, call our function. */
STATIC int
xfs_iwalk_ag_recs(
	struct xfs_iwalk_ag		*iwag)
{
	struct xfs_inobt_rec_incore	*irec;
	xfs_ino_t			ino;
	unsigned int			i, j;
	xfs_agnumber_t			agno;
	int				error;

	agno = XFS_INO_TO_AGNO(iwag->mp, iwag->startino);
	for (i = 0, irec = iwag->recs; i < iwag->nr_recs; i++, irec++) {
		trace_xfs_iwalk_ag_rec(iwag->mp, agno, irec->ir_startino,
				irec->ir_free);
		for (j = 0; j < XFS_INODES_PER_CHUNK; j++) {
			/* Skip if this inode is free */
			if (XFS_INOBT_MASK(j) & irec->ir_free)
				continue;

			/* Otherwise call our function. */
			ino = XFS_AGINO_TO_INO(iwag->mp, agno,
					irec->ir_startino + j);
			error = iwag->iwalk_fn(iwag->mp, ino, iwag->data);
			if (error)
				return error;
		}
	}

	iwag->nr_recs = 0;
	return 0;
}

/* Read AGI and create inobt cursor. */
static inline struct xfs_btree_cur *
xfs_iwalk_inobt_cur(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct xfs_buf		**agi_bpp)
{
	int			error;

	ASSERT(*agi_bpp == NULL);

	error = xfs_ialloc_read_agi(mp, NULL, agno, agi_bpp);
	if (error)
		return ERR_PTR(error);

	return xfs_inobt_init_cursor(mp, NULL, *agi_bpp, agno, XFS_BTNUM_INO);
}

/* Delete cursor and let go of AGI. */
static inline void
xfs_iwalk_del_inobt(
	struct xfs_btree_cur	**curpp,
	struct xfs_buf		**agi_bpp,
	int			error)
{
	xfs_btree_del_cursor(*curpp, error);
	*curpp = NULL;
	xfs_buf_relse(*agi_bpp);
	*agi_bpp = NULL;
}

/* Walk all inodes in a single AG, from @iwag->startino to the end of the AG. */
STATIC int
xfs_iwalk_ag(
	struct xfs_iwalk_ag		*iwag)
{
	struct xfs_mount		*mp = iwag->mp;
	struct xfs_buf			*agi_bp = NULL;
	struct xfs_btree_cur		*cur;
	xfs_agnumber_t			agno;
	xfs_agino_t			agino;
	int				has_rec;
	int				error = 0;

	agno = XFS_INO_TO_AGNO(mp, iwag->startino);
	agino = XFS_INO_TO_AGINO(mp, iwag->startino);

	cur = xfs_iwalk_inobt_cur(mp, agno, &agi_bp);
	if (IS_ERR(cur))
		return PTR_ERR(cur);

	/*
	 * If caller passed in a nonzero start inode number, load the record
	 * from the inobt and make the record look like all the inodes before
	 * agino are free so that we skip them, and then move the cursor to the
	 * next inobt record.  This is how we support starting an iwalk in the
	 * middle of an inode chunk.
	 *
	 * If the caller passed in a start number of zero, move the cursor to
	 * the first inobt record.
	 */
	if (agino != 0) {
		int			icount;

		error = xfs_iwalk_grab_ichunk(cur, agino, &icount,
				&iwag->recs[iwag->nr_recs]);
		if (error)
			goto out_cur;
		if (icount)
			iwag->nr_recs++;

		error = xfs_btree_increment(cur, 0, &has_rec);
	} else {
		error = xfs_inobt_lookup(cur, 0, XFS_LOOKUP_GE, &has_rec);
	}
	if (error)
		goto out_cur;

	while (has_rec) {
		struct xfs_inobt_rec_incore	*irec;

		/* Fetch the inobt record. */
		irec = &iwag->recs[iwag->nr_recs];
		error = xfs_inobt_get_rec(cur, irec, &has_rec);
		if (error)
			goto out_cur;
		if (!has_rec)
			break;

		/* No allocated inodes in this chunk; skip it. */
		if (irec->ir_freecount == irec->ir_count) {
			error = xfs_btree_increment(cur, 0, &has_rec);
			goto next_loop;
		}

		/*
		 * If this chunk has any allocated inodes, save it and start
		 * read-ahead now for this chunk.
		 */
		iwag->nr_recs++;
		xfs_iwalk_ichunk_ra(mp, agno, irec);

		/* If the record cache is full, walk the records. */
		if (iwag->nr_recs == iwag->sz_recs) {
			xfs_iwalk_del_inobt(&cur, &agi_bp, error);

			error = xfs_iwalk_ag_recs(iwag);
			if (error)
				return error;

			/* Recreate cursor where we left off. */
			cur = xfs_iwalk_inobt_cur(mp, agno, &agi_bp);
			if (IS_ERR(cur))
				return PTR_ERR(cur);

			error = xfs_inobt_lookup(cur, irec->ir_startino +
					XFS_INODES_PER_CHUNK - 1,
					XFS_LOOKUP_GE, &has_rec);
		} else {
			/* Move on to the next chunk. */
			error = xfs_btree_increment(cur, 0, &has_rec);
		}
next_loop:
		if (error)
			goto out_cur;
		cond_resched();
	}

	/* Walk any records left behind in the cache. */
	if (iwag->nr_recs) {
		xfs_iwalk_del_inobt(&cur, &agi_bp, error);
		return xfs_iwalk_ag_recs(iwag);
	}

out_cur:
	xfs_iwalk_del_inobt(&cur, &agi_bp, error);
	return error;
}

/* Walk all inodes in the filesystem starting from @startino. */
int
xfs_iwalk(
	struct xfs_mount	*mp,
	xfs_ino_t		startino,
	xfs_iwalk_fn		iwalk_fn,
	void			*data)
{
	struct xfs_iwalk_ag	iwag = {
		.mp		= mp,
		.iwalk_fn	= iwalk_fn,
		.data		= data,
		.startino	= startino,
	};
	xfs_agnumber_t		agno;
	int			error;

	if (startino && !xfs_verify_ino(mp, startino))
		return -EINVAL;

	error = xfs_iwalk_alloc(&iwag);
	if (error)
		return error;

	for (agno = XFS_INO_TO_AGNO(mp, startino);
	     agno < mp->m_sb.sb_agcount;
	     agno++) {
		error = xfs_iwalk_ag(&iwag);
		if (error)
			break;
		iwag.startino = XFS_AGINO_TO_INO(mp, agno + 1, 0);
	}

	xfs_iwalk_free(&iwag);
	return error;
}
