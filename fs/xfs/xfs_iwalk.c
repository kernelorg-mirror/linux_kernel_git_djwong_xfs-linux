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
 * Walking Inodes in the Filesystem
 * ================================
 *
 * This iterator function walks a subset of filesystem inodes in increasing
 * order from @startino until there are no more inodes.  For each allocated
 * inode it finds, it calls a walk function with the relevant inode number and
 * a pointer to caller-provided data.  The walk function can return the usual
 * negative error code to stop the iteration; 0 to continue the iteration; or
 * XFS_IWALK_ABORT to stop the iteration.  This return value is returned to the
 * caller.
 *
 * Internally, we allow the walk function to do anything, which means that we
 * cannot maintain the inobt cursor or our lock on the AGI buffer.  We
 * therefore cache the inobt records in kernel memory and only call the walk
 * function when our memory buffer is full.  @nr_recs is the number of records
 * that we've cached, and @sz_recs is the size of our cache.
 *
 * It is the responsibility of the walk function to ensure it accesses
 * allocated inodes, as the inobt records may be stale by the time they are
 * acted upon.
 */

struct xfs_iwalk_ag {
	/* parallel work control data; will be null if single threaded */
	struct xfs_pwork		pwork;

	struct xfs_mount		*mp;
	struct xfs_trans		*tp;

	/* Where do we start the traversal? */
	xfs_ino_t			startino;

	/* Array of inobt records we cache. */
	struct xfs_inobt_rec_incore	*recs;

	/* Number of entries allocated for the @recs array. */
	unsigned int			sz_recs;

	/* Number of entries in the @recs array that are in use. */
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
	struct xfs_ino_geometry		*igeo = M_IGEO(mp);
	xfs_agblock_t			agbno;
	struct blk_plug			plug;
	int				i;	/* inode chunk index */

	agbno = XFS_AGINO_TO_AGBNO(mp, irec->ir_startino);

	blk_start_plug(&plug);
	for (i = 0; i < XFS_INODES_PER_CHUNK; i += igeo->inodes_per_cluster) {
		xfs_inofree_t	imask;

		imask = xfs_inobt_maskn(i, igeo->inodes_per_cluster);
		if (imask & ~irec->ir_free) {
			xfs_btree_reada_bufs(mp, agno, agbno,
					igeo->blocks_per_cluster,
					&xfs_inode_buf_ops);
		}
		agbno += igeo->blocks_per_cluster;
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
	iwag->recs = kmem_alloc(size, KM_MAYFAIL);
	if (iwag->recs == NULL)
		return -ENOMEM;

	return 0;
}

/* Free memory we allocated for a walk. */
STATIC void
xfs_iwalk_free(
	struct xfs_iwalk_ag	*iwag)
{
	kmem_free(iwag->recs);
}

/* For each inuse inode in each cached inobt record, call our function. */
STATIC int
xfs_iwalk_ag_recs(
	struct xfs_iwalk_ag		*iwag)
{
	struct xfs_mount		*mp = iwag->mp;
	struct xfs_trans		*tp = iwag->tp;
	xfs_ino_t			ino;
	unsigned int			i, j;
	xfs_agnumber_t			agno;
	int				error;

	agno = XFS_INO_TO_AGNO(mp, iwag->startino);
	for (i = 0; i < iwag->nr_recs; i++) {
		struct xfs_inobt_rec_incore	*irec = &iwag->recs[i];

		trace_xfs_iwalk_ag_rec(mp, agno, irec);

		for (j = 0; j < XFS_INODES_PER_CHUNK; j++) {
			if (xfs_pwork_want_abort(&iwag->pwork))
				return 0;

			/* Skip if this inode is free */
			if (XFS_INOBT_MASK(j) & irec->ir_free)
				continue;

			/* Otherwise call our function. */
			ino = XFS_AGINO_TO_INO(mp, agno, irec->ir_startino + j);
			error = iwag->iwalk_fn(mp, tp, ino, iwag->data);
			if (error)
				return error;
		}
	}

	return 0;
}

/* Delete cursor and let go of AGI. */
static inline void
xfs_iwalk_del_inobt(
	struct xfs_trans	*tp,
	struct xfs_btree_cur	**curpp,
	struct xfs_buf		**agi_bpp,
	int			error)
{
	if (*curpp) {
		xfs_btree_del_cursor(*curpp, error);
		*curpp = NULL;
	}
	if (*agi_bpp) {
		xfs_trans_brelse(tp, *agi_bpp);
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
	struct xfs_iwalk_ag	*iwag,
	xfs_agnumber_t		agno,
	xfs_agino_t		agino,
	struct xfs_btree_cur	**curpp,
	struct xfs_buf		**agi_bpp,
	int			*has_more,
	bool			trim)
{
	struct xfs_mount	*mp = iwag->mp;
	struct xfs_trans	*tp = iwag->tp;
	int			icount;
	int			error;

	/* Set up a fresh cursor and empty the inobt cache. */
	iwag->nr_recs = 0;
	error = xfs_inobt_cur(mp, tp, agno, curpp, agi_bpp);
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

	/*
	 * set_prefetch is supposed to give us a large enough inobt record
	 * cache that grab_ichunk can stage a partial first record and the loop
	 * body can cache a record without having to check for cache space
	 * until after it reads an inobt record.
	 */
	ASSERT(iwag->nr_recs < iwag->sz_recs);

	return xfs_btree_increment(*curpp, 0, has_more);
}

typedef int (*xfs_iwalk_ag_recs_fn)(struct xfs_iwalk_ag *iwag);

/*
 * The inobt record cache is full, so preserve the inobt cursor state and
 * run callbacks on the cached inobt records.  When we're done, restore the
 * cursor state to wherever the cursor would have been had the cache not been
 * full (and therefore we could've just incremented the cursor).
 */
STATIC int
xfs_iwalk_run_callbacks(
	struct xfs_iwalk_ag		*iwag,
	xfs_iwalk_ag_recs_fn		walk_ag_recs_fn,
	xfs_agnumber_t			agno,
	struct xfs_btree_cur		**curpp,
	struct xfs_buf			**agi_bpp,
	int				*has_more)
{
	struct xfs_mount		*mp = iwag->mp;
	struct xfs_trans		*tp = iwag->tp;
	struct xfs_inobt_rec_incore	*irec;
	xfs_agino_t			restart;
	int				error;

	ASSERT(iwag->nr_recs == iwag->sz_recs);

	/* Delete cursor but remember the last record we cached... */
	xfs_iwalk_del_inobt(tp, curpp, agi_bpp, 0);
	irec = &iwag->recs[iwag->nr_recs - 1];
	restart = irec->ir_startino + XFS_INODES_PER_CHUNK - 1;

	error = walk_ag_recs_fn(iwag);
	if (error)
		return error;

	/* ...empty the cache... */
	iwag->nr_recs = 0;

	/* ...and recreate the cursor just past where we left off. */
	error = xfs_inobt_cur(mp, tp, agno, curpp, agi_bpp);
	if (error)
		return error;

	return xfs_inobt_lookup(*curpp, restart, XFS_LOOKUP_GE, has_more);
}

/* Walk all inodes in a single AG, from @iwag->startino to the end of the AG. */
STATIC int
xfs_iwalk_ag(
	struct xfs_iwalk_ag		*iwag)
{
	struct xfs_mount		*mp = iwag->mp;
	struct xfs_trans		*tp = iwag->tp;
	struct xfs_buf			*agi_bp = NULL;
	struct xfs_btree_cur		*cur = NULL;
	xfs_agnumber_t			agno;
	xfs_agino_t			agino;
	int				has_more;
	int				error = 0;

	/* Set up our cursor at the right place in the inode btree. */
	agno = XFS_INO_TO_AGNO(mp, iwag->startino);
	agino = XFS_INO_TO_AGINO(mp, iwag->startino);
	error = xfs_iwalk_ag_start(iwag, agno, agino, &cur, &agi_bp, &has_more,
			true);

	while (!error && has_more && !xfs_pwork_want_abort(&iwag->pwork)) {
		struct xfs_inobt_rec_incore	*irec;

		cond_resched();

		/* Fetch the inobt record. */
		irec = &iwag->recs[iwag->nr_recs];
		error = xfs_inobt_get_rec(cur, irec, &has_more);
		if (error || !has_more)
			break;

		/* No allocated inodes in this chunk; skip it. */
		if (irec->ir_freecount == irec->ir_count) {
			error = xfs_btree_increment(cur, 0, &has_more);
			if (error)
				break;
			continue;
		}

		/*
		 * Start readahead for this inode chunk in anticipation of
		 * walking the inodes.
		 */
		xfs_iwalk_ichunk_ra(mp, agno, irec);

		/*
		 * If there's space in the buffer for more records, increment
		 * the btree cursor and grab more.
		 */
		if (++iwag->nr_recs < iwag->sz_recs) {
			error = xfs_btree_increment(cur, 0, &has_more);
			if (error || !has_more)
				break;
			continue;
		}

		/*
		 * Otherwise, we need to save cursor state and run the callback
		 * function on the cached records.  The run_callbacks function
		 * is supposed to return a cursor pointing to the record where
		 * we would be if we had been able to increment like above.
		 */
		error = xfs_iwalk_run_callbacks(iwag, xfs_iwalk_ag_recs, agno,
				&cur, &agi_bp, &has_more);
	}

	xfs_iwalk_del_inobt(tp, &cur, &agi_bp, error);

	/* Walk any records left behind in the cache. */
	if (iwag->nr_recs == 0 || error || xfs_pwork_want_abort(&iwag->pwork))
		return error;

	return xfs_iwalk_ag_recs(iwag);
}

/*
 * Given the number of inodes to prefetch, set the number of inobt records that
 * we cache in memory, which controls the number of inodes we try to read
 * ahead.
 *
 * If no max prefetch was given, default to 4096 bytes' worth of inobt records;
 * this should be plenty of inodes to read ahead.  This number (256 inobt
 * records) was chosen so that the cache is never more than a single memory
 * page.
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
		iwag->sz_recs = 4096 / sizeof(struct xfs_inobt_rec_incore);

	/*
	 * Allocate enough space to prefetch at least two records so that we
	 * can cache both the inobt record where the iwalk started and the next
	 * record.  This simplifies the AG inode walk loop setup code.
	 */
	iwag->sz_recs = max_t(unsigned int, iwag->sz_recs, 2);
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
	struct xfs_trans	*tp,
	xfs_ino_t		startino,
	unsigned int		flags,
	xfs_iwalk_fn		iwalk_fn,
	unsigned int		max_prefetch,
	void			*data)
{
	struct xfs_iwalk_ag	iwag = {
		.mp		= mp,
		.tp		= tp,
		.iwalk_fn	= iwalk_fn,
		.data		= data,
		.startino	= startino,
		.pwork		= XFS_PWORK_SINGLE_THREADED,
	};
	xfs_agnumber_t		agno = XFS_INO_TO_AGNO(mp, startino);
	int			error;

	ASSERT(agno < mp->m_sb.sb_agcount);
	ASSERT(!(flags & ~XFS_IWALK_FLAGS_ALL));

	xfs_iwalk_set_prefetch(&iwag, max_prefetch);
	error = xfs_iwalk_alloc(&iwag);
	if (error)
		return error;

	for (; agno < mp->m_sb.sb_agcount; agno++) {
		error = xfs_iwalk_ag(&iwag);
		if (error)
			break;
		iwag.startino = XFS_AGINO_TO_INO(mp, agno + 1, 0);
		if (flags & XFS_INOBT_WALK_SAME_AG)
			break;
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

	error = xfs_iwalk_ag(iwag);
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
	unsigned int		flags,
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
	ASSERT(!(flags & ~XFS_IWALK_FLAGS_ALL));

	nr_threads = xfs_pwork_guess_datadev_parallelism(mp);
	error = xfs_pwork_init(mp, &pctl, xfs_iwalk_ag_work, "xfs_iwalk",
			nr_threads);
	if (error)
		return error;

	for (; agno < mp->m_sb.sb_agcount; agno++) {
		struct xfs_iwalk_ag	*iwag;

		iwag = kmem_alloc(sizeof(struct xfs_iwalk_ag), KM_SLEEP);
		iwag->mp = mp;
		iwag->tp = NULL;
		iwag->iwalk_fn = iwalk_fn;
		iwag->data = data;
		iwag->startino = startino;
		iwag->recs = NULL;
		xfs_iwalk_set_prefetch(iwag, max_prefetch);
		xfs_pwork_queue(&pctl, &iwag->pwork);
		startino = XFS_AGINO_TO_INO(mp, agno + 1, 0);
		if (flags & XFS_INOBT_WALK_SAME_AG)
			break;
	}

	if (polled)
		return xfs_pwork_destroy_poll(&pctl);
	return xfs_pwork_destroy(&pctl);
}

/* For each inuse inode in each cached inobt record, call our function. */
STATIC int
xfs_inobt_walk_ag_recs(
	struct xfs_iwalk_ag		*iwag)
{
	struct xfs_mount		*mp = iwag->mp;
	struct xfs_trans		*tp = iwag->tp;
	struct xfs_inobt_rec_incore	*irec;
	unsigned int			i;
	xfs_agnumber_t			agno;
	int				error;

	agno = XFS_INO_TO_AGNO(mp, iwag->startino);
	for (i = 0, irec = iwag->recs; i < iwag->nr_recs; i++, irec++) {
		trace_xfs_iwalk_ag_rec(mp, agno, irec);
		error = iwag->inobt_walk_fn(mp, tp, agno, irec, iwag->data);
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
	struct xfs_iwalk_ag		*iwag)
{
	struct xfs_mount		*mp = iwag->mp;
	struct xfs_trans		*tp = iwag->tp;
	struct xfs_buf			*agi_bp = NULL;
	struct xfs_btree_cur		*cur = NULL;
	xfs_agnumber_t			agno;
	xfs_agino_t			agino;
	int				has_more;
	int				error = 0;

	/* Set up our cursor at the right place in the inode btree. */
	agno = XFS_INO_TO_AGNO(mp, iwag->startino);
	agino = XFS_INO_TO_AGINO(mp, iwag->startino);
	error = xfs_iwalk_ag_start(iwag, agno, agino, &cur, &agi_bp, &has_more,
			false);

	while (!error && has_more && !xfs_pwork_want_abort(&iwag->pwork)) {
		struct xfs_inobt_rec_incore	*irec;

		cond_resched();

		/* Fetch the inobt record. */
		irec = &iwag->recs[iwag->nr_recs];
		error = xfs_inobt_get_rec(cur, irec, &has_more);
		if (error || !has_more)
			break;

		/*
		 * If there's space in the buffer for more records, increment
		 * the btree cursor and grab more.
		 */
		if (++iwag->nr_recs < iwag->sz_recs) {
			error = xfs_btree_increment(cur, 0, &has_more);
			if (error || !has_more)
				break;
			continue;
		}

		/*
		 * Otherwise, we need to save cursor state and run the callback
		 * function on the cached records.  The run_callbacks function
		 * is supposed to return a cursor pointing to the record where
		 * we would be if we had been able to increment like above.
		 */
		error = xfs_iwalk_run_callbacks(iwag, xfs_inobt_walk_ag_recs,
				agno, &cur, &agi_bp, &has_more);
	}

	xfs_iwalk_del_inobt(tp, &cur, &agi_bp, error);

	/* Walk any records left behind in the cache. */
	if (iwag->nr_recs == 0 || error || xfs_pwork_want_abort(&iwag->pwork))
		return error;

	return xfs_inobt_walk_ag_recs(iwag);
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
	struct xfs_trans	*tp,
	xfs_ino_t		startino,
	unsigned int		flags,
	xfs_inobt_walk_fn	inobt_walk_fn,
	unsigned int		max_prefetch,
	void			*data)
{
	struct xfs_iwalk_ag	iwag = {
		.mp		= mp,
		.tp		= tp,
		.inobt_walk_fn	= inobt_walk_fn,
		.data		= data,
		.startino	= startino,
		.pwork		= XFS_PWORK_SINGLE_THREADED,
	};
	xfs_agnumber_t		agno = XFS_INO_TO_AGNO(mp, startino);
	int			error;

	ASSERT(agno < mp->m_sb.sb_agcount);
	ASSERT(!(flags & ~XFS_INOBT_WALK_FLAGS_ALL));

	xfs_iwalk_set_prefetch(&iwag, max_prefetch * XFS_INODES_PER_CHUNK);
	error = xfs_iwalk_alloc(&iwag);
	if (error)
		return error;

	for (; agno < mp->m_sb.sb_agcount; agno++) {
		error = xfs_inobt_walk_ag(&iwag);
		if (error)
			break;
		iwag.startino = XFS_AGINO_TO_INO(mp, agno + 1, 0);
		if (flags & XFS_INOBT_WALK_SAME_AG)
			break;
	}

	xfs_iwalk_free(&iwag);
	return error;
}
