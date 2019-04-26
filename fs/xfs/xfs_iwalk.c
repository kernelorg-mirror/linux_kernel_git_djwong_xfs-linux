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
#include "xfs_pwork.h"

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
	/* parallel work control data; will be null if single threaded */
	struct xfs_pwork		pwork;

	/* Where do we start the traversal? */
	xfs_ino_t			startino;

	/* Array of inobt records we cache. */
	struct xfs_inobt_rec_incore	*recs;
	unsigned int			sz_recs;
	unsigned int			nr_recs;

	/* Inode walk function and data pointer. */
	union {
		xfs_iwalk_fn		iwalk_fn;
		xfs_inobt_walk_fn	inobt_walk_fn;
	};
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
		xfs_inofree_t	imask;

		imask = xfs_inobt_maskn(i, igeo->ig_inodes_per_cluster);
		if (imask & ~irec->ir_free) {
			xfs_btree_reada_bufs(mp, agno, agbno,
					igeo->ig_blocks_per_cluster,
					&xfs_inode_buf_ops);
		}
	}
	blk_finish_plug(&plug);
}

/*
 * Lookup the inode chunk that the given @agino lives in and then get the
 * record if we found the chunk.  If @trim is set, set the bits in @irec's free
 * mask that correspond to the inodes before @agino so that we skip them.
 * This is how we restart an inode walk that was interrupted in the middle of
 * an inode record.
 */
STATIC int
xfs_iwalk_grab_ichunk(
	struct xfs_btree_cur		*cur,	/* btree cursor */
	xfs_agino_t			agino,	/* starting inode of chunk */
	int				*icount,/* return # of inodes grabbed */
	struct xfs_inobt_rec_incore	*irec,	/* btree record */
	bool				trim)
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

	/* Return the entire record if the caller wants the whole thing. */
	if (!trim) {
		*icount = irec->ir_count;
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

	/* Allocate a prefetch buffer for inobt records. */
	size = iwag->sz_recs * sizeof(struct xfs_inobt_rec_incore);
	iwag->recs = kmem_alloc(size, KM_SLEEP);
	if (iwag->recs == NULL)
		return -ENOMEM;

	return 0;
}

/* Free memory we allocated for a walk. */
STATIC void
xfs_iwalk_free(
	struct xfs_iwalk_ag	*iwag)
{
	ASSERT(iwag->recs != NULL);
	kmem_free(iwag->recs);
}

/* For each inuse inode in each cached inobt record, call our function. */
STATIC int
xfs_iwalk_ag_recs(
	struct xfs_mount		*mp,
	struct xfs_iwalk_ag		*iwag)
{
	struct xfs_inobt_rec_incore	*irec;
	xfs_ino_t			ino;
	unsigned int			i, j;
	xfs_agnumber_t			agno;
	int				error;

	agno = XFS_INO_TO_AGNO(mp, iwag->startino);
	for (i = 0, irec = iwag->recs; i < iwag->nr_recs; i++, irec++) {
		trace_xfs_iwalk_ag_rec(mp, agno, irec->ir_startino,
				irec->ir_free);
		for (j = 0; j < XFS_INODES_PER_CHUNK; j++) {
			if (xfs_pwork_want_abort(&iwag->pwork))
				return 0;

			/* Skip if this inode is free */
			if (XFS_INOBT_MASK(j) & irec->ir_free)
				continue;

			/* Otherwise call our function. */
			ino = XFS_AGINO_TO_INO(mp, agno, irec->ir_startino + j);
			error = iwag->iwalk_fn(mp, ino, iwag->data);
			if (error)
				return error;
		}
	}

	iwag->nr_recs = 0;
	return 0;
}

/* Read AGI and create inobt cursor. */
static inline int
xfs_iwalk_inobt_cur(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct xfs_btree_cur	**curpp,
	struct xfs_buf		**agi_bpp)
{
	struct xfs_btree_cur	*cur;
	int			error;

	ASSERT(*agi_bpp == NULL);

	error = xfs_ialloc_read_agi(mp, NULL, agno, agi_bpp);
	if (error)
		return error;

	cur = xfs_inobt_init_cursor(mp, NULL, *agi_bpp, agno, XFS_BTNUM_INO);
	if (!cur)
		return -ENOMEM;
	*curpp = cur;
	return 0;
}

/* Delete cursor and let go of AGI. */
static inline void
xfs_iwalk_del_inobt(
	struct xfs_btree_cur	**curpp,
	struct xfs_buf		**agi_bpp,
	int			error)
{
	if (*curpp) {
		xfs_btree_del_cursor(*curpp, error);
		*curpp = NULL;
	}
	if (*agi_bpp) {
		xfs_buf_relse(*agi_bpp);
		*agi_bpp = NULL;
	}
}

/*
 * Set ourselves up for walking inobt records starting from a given point in
 * the filesystem.
 *
 * If caller passed in a nonzero start inode number, load the record from the
 * inobt and make the record look like all the inodes before agino are free so
 * that we skip them, and then move the cursor to the next inobt record.  This
 * is how we support starting an iwalk in the middle of an inode chunk.
 *
 * If the caller passed in a start number of zero, move the cursor to the first
 * inobt record.
 *
 * The caller is responsible for cleaning up the cursor and buffer pointer
 * regardless of the error status.
 */
STATIC int
xfs_iwalk_ag_start(
	struct xfs_mount	*mp,
	struct xfs_iwalk_ag	*iwag,
	xfs_agnumber_t		agno,
	xfs_agino_t		agino,
	struct xfs_btree_cur	**curpp,
	struct xfs_buf		**agi_bpp,
	int			*has_more,
	bool			trim)
{
	int			icount;
	int			error;

	/* Set up a fresh cursor and empty the inobt cache. */
	iwag->nr_recs = 0;
	error = xfs_iwalk_inobt_cur(mp, agno, curpp, agi_bpp);
	if (error)
		return error;

	/* Starting at the beginning of the AG?  That's easy! */
	if (agino == 0)
		return xfs_inobt_lookup(*curpp, 0, XFS_LOOKUP_GE, has_more);

	/*
	 * Otherwise, we have to grab the inobt record where we left off, stuff
	 * the record into our cache, and then see if there are more records.
	 * We require a lookup cache of at least two elements so that we don't
	 * have to deal with tearing down the cursor to walk the records.
	 */
	error = xfs_iwalk_grab_ichunk(*curpp, agino, &icount,
			&iwag->recs[iwag->nr_recs], trim);
	if (error)
		return error;
	if (icount)
		iwag->nr_recs++;

	ASSERT(iwag->nr_recs < iwag->sz_recs);
	return xfs_btree_increment(*curpp, 0, has_more);
}

typedef int (*xfs_iwalk_recs_fn)(struct xfs_mount *mp,
		struct xfs_iwalk_ag *iwag);

/*
 * Acknowledge that we added an inobt record to the cache.  Flush the inobt
 * record cache if the buffer is full, and position the cursor wherever it
 * needs to be so that we can keep going.
 */
STATIC int
xfs_iwalk_ag_increment(
	struct xfs_mount	*mp,
	struct xfs_iwalk_ag	*iwag,
	xfs_iwalk_recs_fn	walk_recs_fn,
	xfs_agnumber_t		agno,
	struct xfs_btree_cur	**curpp,
	struct xfs_buf		**agi_bpp,
	int			*has_more)
{
	struct xfs_inobt_rec_incore *irec;
	xfs_agino_t		restart;
	int			error;

	iwag->nr_recs++;

	/* If there's space, just increment and look for more records. */
	if (iwag->nr_recs < iwag->sz_recs)
		return xfs_btree_increment(*curpp, 0, has_more);

	/*
	 * Otherwise the record cache is full; delete the cursor and walk the
	 * records...
	 */
	xfs_iwalk_del_inobt(curpp, agi_bpp, 0);
	irec = &iwag->recs[iwag->nr_recs - 1];
	restart = irec->ir_startino + XFS_INODES_PER_CHUNK - 1;

	error = walk_recs_fn(mp, iwag);
	if (error)
		return error;

	/* ...and recreate cursor where we left off. */
	error = xfs_iwalk_inobt_cur(mp, agno, curpp, agi_bpp);
	if (error)
		return error;

	return xfs_inobt_lookup(*curpp, restart, XFS_LOOKUP_GE, has_more);
}

/* Walk all inodes in a single AG, from @iwag->startino to the end of the AG. */
STATIC int
xfs_iwalk_ag(
	struct xfs_mount		*mp,
	struct xfs_iwalk_ag		*iwag)
{
	struct xfs_buf			*agi_bp = NULL;
	struct xfs_btree_cur		*cur = NULL;
	xfs_agnumber_t			agno;
	xfs_agino_t			agino;
	int				has_more;
	int				error = 0;

	/* Set up our cursor at the right place in the inode btree. */
	agno = XFS_INO_TO_AGNO(mp, iwag->startino);
	agino = XFS_INO_TO_AGINO(mp, iwag->startino);
	error = xfs_iwalk_ag_start(mp, iwag, agno, agino, &cur, &agi_bp,
			&has_more, true);
	if (error)
		goto out_cur;

	while (has_more && !xfs_pwork_want_abort(&iwag->pwork)) {
		struct xfs_inobt_rec_incore	*irec;

		/* Fetch the inobt record. */
		irec = &iwag->recs[iwag->nr_recs];
		error = xfs_inobt_get_rec(cur, irec, &has_more);
		if (error)
			goto out_cur;
		if (!has_more)
			break;

		/* No allocated inodes in this chunk; skip it. */
		if (irec->ir_freecount == irec->ir_count) {
			error = xfs_btree_increment(cur, 0, &has_more);
			goto next_loop;
		}

		/*
		 * Start readahead for this inode chunk in anticipation of
		 * walking the inodes.
		 */
		xfs_iwalk_ichunk_ra(mp, agno, irec);

		/*
		 * Add this inobt record to our cache, flush the cache if
		 * needed, and move on to the next record.
		 */
		error = xfs_iwalk_ag_increment(mp, iwag, xfs_iwalk_ag_recs,
				agno, &cur, &agi_bp, &has_more);
next_loop:
		if (error)
			goto out_cur;
		cond_resched();
	}

	/* Walk any records left behind in the cache. */
	if (iwag->nr_recs && !xfs_pwork_want_abort(&iwag->pwork)) {
		xfs_iwalk_del_inobt(&cur, &agi_bp, error);
		return xfs_iwalk_ag_recs(mp, iwag);
	}

out_cur:
	xfs_iwalk_del_inobt(&cur, &agi_bp, error);
	return error;
}

/*
 * Given the number of inodes to prefetch, set the number of inobt records that
 * we cache in memory, which controls the number of inodes we try to read
 * ahead.
 *
 * If no max prefetch was given, default to one page's worth of inobt records;
 * this should be plenty of inodes to read ahead.
 */
static inline void
xfs_iwalk_set_prefetch(
	struct xfs_iwalk_ag	*iwag,
	unsigned int		max_prefetch)
{
	if (max_prefetch)
		iwag->sz_recs = round_up(max_prefetch, XFS_INODES_PER_CHUNK) /
					XFS_INODES_PER_CHUNK;
	else
		iwag->sz_recs = PAGE_SIZE / sizeof(struct xfs_inobt_rec_incore);

	/*
	 * Allocate enough space to prefetch at least two records so that we
	 * can cache both the inobt record where the iwalk started and the next
	 * record.  This simplifies the AG inode walk loop setup code.
	 */
	if (iwag->sz_recs < 2)
		iwag->sz_recs = 2;
}

/*
 * Walk all inodes in the filesystem starting from @startino.  The @iwalk_fn
 * will be called for each allocated inode, being passed the inode's number and
 * @data.  @max_prefetch controls how many inobt records' worth of inodes we
 * try to readahead.
 */
int
xfs_iwalk(
	struct xfs_mount	*mp,
	xfs_ino_t		startino,
	xfs_iwalk_fn		iwalk_fn,
	unsigned int		max_prefetch,
	void			*data)
{
	struct xfs_iwalk_ag	iwag = {
		.iwalk_fn	= iwalk_fn,
		.data		= data,
		.startino	= startino,
		.pwork		= XFS_PWORK_SINGLE_THREADED,
	};
	xfs_agnumber_t		agno = XFS_INO_TO_AGNO(mp, startino);
	int			error;

	ASSERT(agno < mp->m_sb.sb_agcount);

	xfs_iwalk_set_prefetch(&iwag, max_prefetch);
	error = xfs_iwalk_alloc(&iwag);
	if (error)
		return error;

	for (; agno < mp->m_sb.sb_agcount; agno++) {
		error = xfs_iwalk_ag(mp, &iwag);
		if (error)
			break;
		iwag.startino = XFS_AGINO_TO_INO(mp, agno + 1, 0);
	}

	xfs_iwalk_free(&iwag);
	return error;
}

/* Run per-thread iwalk work. */
static int
xfs_iwalk_ag_work(
	struct xfs_mount	*mp,
	struct xfs_pwork	*pwork)
{
	struct xfs_iwalk_ag	*iwag;
	int			error;

	iwag = container_of(pwork, struct xfs_iwalk_ag, pwork);
	error = xfs_iwalk_alloc(iwag);
	if (error)
		goto out;

	error = xfs_iwalk_ag(mp, iwag);
	xfs_iwalk_free(iwag);
out:
	kmem_free(iwag);
	return error;
}

/*
 * Walk all the inodes in the filesystem using multiple threads to process each
 * AG.
 */
int
xfs_iwalk_threaded(
	struct xfs_mount	*mp,
	xfs_ino_t		startino,
	xfs_iwalk_fn		iwalk_fn,
	unsigned int		max_prefetch,
	bool			polled,
	void			*data)
{
	struct xfs_pwork_ctl	pctl;
	xfs_agnumber_t		agno = XFS_INO_TO_AGNO(mp, startino);
	unsigned int		nr_threads;
	int			error;

	ASSERT(agno < mp->m_sb.sb_agcount);

	nr_threads = xfs_pwork_guess_datadev_parallelism(mp);
	error = xfs_pwork_init(mp, &pctl, xfs_iwalk_ag_work, "xfs_iwalk",
			nr_threads);
	if (error)
		return error;

	for (; agno < mp->m_sb.sb_agcount; agno++) {
		struct xfs_iwalk_ag	*iwag;

		iwag = kmem_alloc(sizeof(struct xfs_iwalk_ag), KM_SLEEP);
		iwag->iwalk_fn = iwalk_fn;
		iwag->data = data;
		iwag->startino = startino;
		iwag->recs = NULL;
		xfs_iwalk_set_prefetch(iwag, max_prefetch);
		xfs_pwork_queue(&pctl, &iwag->pwork);
		startino = XFS_AGINO_TO_INO(mp, agno + 1, 0);
	}

	if (polled)
		return xfs_pwork_destroy_poll(&pctl);
	return xfs_pwork_destroy(&pctl);
}

/* For each inuse inode in each cached inobt record, call our function. */
STATIC int
xfs_inobt_walk_ag_recs(
	struct xfs_mount		*mp,
	struct xfs_iwalk_ag		*iwag)
{
	struct xfs_inobt_rec_incore	*irec;
	unsigned int			i;
	xfs_agnumber_t			agno;
	int				error;

	agno = XFS_INO_TO_AGNO(mp, iwag->startino);
	for (i = 0, irec = iwag->recs; i < iwag->nr_recs; i++, irec++) {
		trace_xfs_iwalk_ag_rec(mp, agno, irec->ir_startino,
				irec->ir_free);
		error = iwag->inobt_walk_fn(mp, agno, irec, iwag->data);
		if (error)
			return error;
	}

	iwag->nr_recs = 0;
	return 0;
}

/*
 * Walk all inode btree records in a single AG, from @iwag->startino to the end
 * of the AG.
 */
STATIC int
xfs_inobt_walk_ag(
	struct xfs_mount		*mp,
	struct xfs_iwalk_ag		*iwag)
{
	struct xfs_buf			*agi_bp = NULL;
	struct xfs_btree_cur		*cur = NULL;
	xfs_agnumber_t			agno;
	xfs_agino_t			agino;
	int				has_more;
	int				error = 0;

	/* Set up our cursor at the right place in the inode btree. */
	agno = XFS_INO_TO_AGNO(mp, iwag->startino);
	agino = XFS_INO_TO_AGINO(mp, iwag->startino);
	error = xfs_iwalk_ag_start(mp, iwag, agno, agino, &cur, &agi_bp,
			&has_more, false);
	if (error)
		goto out_cur;

	while (has_more && !xfs_pwork_want_abort(&iwag->pwork)) {
		struct xfs_inobt_rec_incore	*irec;

		/* Fetch the inobt record. */
		irec = &iwag->recs[iwag->nr_recs];
		error = xfs_inobt_get_rec(cur, irec, &has_more);
		if (error)
			goto out_cur;
		if (!has_more)
			break;

		/*
		 * Add this inobt record to our cache, flush the cache if
		 * needed, and move on to the next record.
		 */
		error = xfs_iwalk_ag_increment(mp, iwag, xfs_inobt_walk_ag_recs,
				agno, &cur, &agi_bp, &has_more);
		if (error)
			goto out_cur;
		cond_resched();
	}

	/* Walk any records left behind in the cache. */
	if (iwag->nr_recs && !xfs_pwork_want_abort(&iwag->pwork)) {
		xfs_iwalk_del_inobt(&cur, &agi_bp, error);
		return xfs_inobt_walk_ag_recs(mp, iwag);
	}

out_cur:
	xfs_iwalk_del_inobt(&cur, &agi_bp, error);
	return error;
}

/*
 * Walk all inode btree records in the filesystem starting from @startino.  The
 * @inobt_walk_fn will be called for each btree record, being passed the incore
 * record and @data.  @max_prefetch controls how many inobt records we try to
 * cache ahead of time.
 */
int
xfs_inobt_walk(
	struct xfs_mount	*mp,
	xfs_ino_t		startino,
	xfs_inobt_walk_fn	inobt_walk_fn,
	unsigned int		max_prefetch,
	void			*data)
{
	struct xfs_iwalk_ag	iwag = {
		.inobt_walk_fn	= inobt_walk_fn,
		.data		= data,
		.startino	= startino,
		.pwork		= XFS_PWORK_SINGLE_THREADED,
	};
	xfs_agnumber_t		agno = XFS_INO_TO_AGNO(mp, startino);
	int			error;

	ASSERT(agno < mp->m_sb.sb_agcount);

	xfs_iwalk_set_prefetch(&iwag, max_prefetch * XFS_INODES_PER_CHUNK);
	error = xfs_iwalk_alloc(&iwag);
	if (error)
		return error;

	for (; agno < mp->m_sb.sb_agcount; agno++) {
		error = xfs_inobt_walk_ag(mp, &iwag);
		if (error)
			break;
		iwag.startino = XFS_AGINO_TO_INO(mp, agno + 1, 0);
	}

	xfs_iwalk_free(&iwag);
	return error;
}
