// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_btree.h"
#include "xfs_btree_mem.h"
#include "xfs_bit.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_alloc.h"
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_quota.h"
#include "xfs_rtalloc.h"
#include "xfs_ag.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/bitmap.h"
#include "scrub/xfile.h"
#include "scrub/iscan.h"
#include "scrub/xfbtree.h"

/*
 * Realtime Reverse Mapping Btree Repair
 * =====================================
 *
 * This isn't quite as difficult as repairing the rmap btree on the data
 * device, since we only store the data fork extents of realtime files on the
 * realtime device.  We still have to freeze the filesystem and stop the
 * background threads like we do for the rmap repair, but we only have to scan
 * realtime inodes.
 *
 * Collecting entries for the new realtime rmap btree is easy -- all we have
 * to do is generate rtrmap entries from the data fork mappings of all realtime
 * files in the filesystem.  We then scan the rmap btrees of the data device
 * looking for extents belonging to the old btree and note them in a bitmap.
 *
 * To rebuild the realtime rmap btree, we bulk-load the collected mappings into
 * a new btree cursor and atomically swap that into the realtime inode.  Then
 * we can free the blocks from the old btree.
 *
 * We use the 'xrep_rtrmap' prefix for all the rmap functions.
 */

/* Set us up to repair rt reverse mapping btrees. */
int
xrep_setup_rtrmapbt(
	struct xfs_scrub	*sc)
{
	int			error;

	error = xfile_create(sc->mp, "rtrmapbt repair", 0, &sc->xfile);
	if (error)
		return error;

	error = xfs_alloc_memory_buftarg(sc->mp, sc->xfile, &sc->xfile_buftarg);
	if (error)
		return error;

	return 0;
}

/* Context for collecting rmaps */
struct xrep_rtrmap {
	/* new rtrmapbt information */
	struct xrep_newbt	new_btree_info;
	struct xfs_btree_bload	rtrmap_bload;

	/* lock for the xfbtree and xfile */
	struct mutex		lock;

	/* rmap records generated from primary metadata */
	struct xfbtree		*rtrmap_btree;
	/* in-memory btree cursor for the ->get_blocks walk */
	struct xfs_btree_cur	*mcur;

	struct xfs_scrub	*sc;

	/* bitmap of old rtrmapbt blocks */
	struct xbitmap		old_rtrmapbt_blocks;

	/* Hooks into rtrmap update code. */
	struct notifier_block	rtrmap_update_hook;

	/* inode scan cursor */
	struct xchk_iscan	iscan;

	/* Number of records we're staging in the new btree. */
	uint64_t		nr_records;
};

/* Make sure there's nothing funny about this mapping. */
STATIC int
xrep_rtrmap_check_mapping(
	struct xfs_scrub	*sc,
	const struct xfs_rmap_irec *rec)
{
	/* Check that this is within the rt volume. */
	if (!xfs_verify_rtext(sc->mp, rec->rm_startblock, rec->rm_blockcount))
		return -EFSCORRUPTED;

	/* Check for a valid fork offset, if applicable. */
	if (!xfs_verify_fileext(sc->mp, rec->rm_offset, rec->rm_blockcount))
		return -EFSCORRUPTED;

	/* Make sure this isn't free space. */
	return xrep_require_rtext_inuse(sc, rec->rm_startblock,
			rec->rm_blockcount);
}

/* Store a reverse-mapping record. */
static inline int
xrep_rtrmap_stash(
	struct xrep_rtrmap	*rr,
	xfs_rtblock_t		startblock,
	xfs_filblks_t		blockcount,
	uint64_t		owner,
	uint64_t		offset,
	unsigned int		flags)
{
	struct xfs_rmap_irec	rmap = {
		.rm_startblock	= startblock,
		.rm_blockcount	= blockcount,
		.rm_owner	= owner,
		.rm_offset	= offset,
		.rm_flags	= flags,
	};
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_btree_cur	*mcur;
	struct xfs_buf		*mhead_bp;
	int			error = 0;

	if (xchk_should_terminate(sc, &error))
		return error;

	if (xchk_iscan_aborted(&rr->iscan))
		return -EFSCORRUPTED;

	trace_xrep_rtrmap_found(sc->mp, &rmap);

	/* Add entry to in-memory btree. */
	mutex_lock(&rr->lock);
	error = xfbtree_head_read_buf(rr->rtrmap_btree, sc->tp, &mhead_bp);
	if (error)
		goto out_abort;

	mcur = xfs_rtrmapbt_mem_cursor(sc->mp, sc->tp, mhead_bp,
			rr->rtrmap_btree);
	error = xfs_rmap_map_raw(mcur, &rmap);
	xfs_btree_del_cursor(mcur, error);
	if (error)
		goto out_cancel;

	error = xfbtree_trans_commit(rr->rtrmap_btree, sc->tp);
	if (error)
		goto out_abort;

	mutex_unlock(&rr->lock);
	return 0;

out_cancel:
	xfbtree_trans_cancel(rr->rtrmap_btree, sc->tp);
out_abort:
	xchk_iscan_abort(&rr->iscan);
	mutex_unlock(&rr->lock);
	return error;
}

/* Finding all file and bmbt extents. */

/* Context for accumulating rmaps for an inode fork. */
struct xrep_rtrmap_ifork {
	/*
	 * Accumulate rmap data here to turn multiple adjacent bmaps into a
	 * single rmap.
	 */
	struct xfs_rmap_irec	accum;

	struct xrep_rtrmap	*rr;
};

/* Stash an rmap that we accumulated while walking an inode fork. */
STATIC int
xrep_rtrmap_stash_accumulated(
	struct xrep_rtrmap_ifork	*rf)
{
	if (rf->accum.rm_blockcount == 0)
		return 0;

	return xrep_rtrmap_stash(rf->rr, rf->accum.rm_startblock,
			rf->accum.rm_blockcount, rf->accum.rm_owner,
			rf->accum.rm_offset, rf->accum.rm_flags);
}

/* Accumulate a bmbt record. */
STATIC int
xrep_rtrmap_visit_bmbt(
	struct xfs_btree_cur	*cur,
	struct xfs_bmbt_irec	*rec,
	void			*priv)
{
	struct xrep_rtrmap_ifork *rf = priv;
	struct xfs_rmap_irec	*accum = &rf->accum;
	xfs_rtblock_t		rtbno;
	unsigned int		rmap_flags = 0;
	int			error;

	rtbno = rec->br_startblock;
	if (rec->br_state == XFS_EXT_UNWRITTEN)
		rmap_flags |= XFS_RMAP_UNWRITTEN;

	/* If this bmap is adjacent to the previous one, just add it. */
	if (accum->rm_blockcount > 0 &&
	    rec->br_startoff == accum->rm_offset + accum->rm_blockcount &&
	    rtbno == accum->rm_startblock + accum->rm_blockcount &&
	    rmap_flags == accum->rm_flags) {
		accum->rm_blockcount += rec->br_blockcount;
		return 0;
	}

	/* Otherwise stash the old rmap and start accumulating a new one. */
	error = xrep_rtrmap_stash_accumulated(rf);
	if (error)
		return error;

	accum->rm_startblock = rtbno;
	accum->rm_blockcount = rec->br_blockcount;
	accum->rm_offset = rec->br_startoff;
	accum->rm_flags = rmap_flags;
	return 0;
}

/*
 * Iterate the block mapping btree to collect rmap records for anything in this
 * fork that maps to the rt volume.  Sets @mappings_done to true if we've
 * scanned the block mappings in this fork.
 */
STATIC int
xrep_rtrmap_scan_bmbt(
	struct xrep_rtrmap_ifork *rf,
	struct xfs_inode	*ip,
	bool			*mappings_done)
{
	struct xrep_rtrmap	*rr = rf->rr;
	struct xfs_btree_cur	*cur;
	struct xfs_ifork	*ifp = XFS_IFORK_PTR(ip, XFS_DATA_FORK);
	int			error = 0;

	*mappings_done = false;

	/*
	 * If the incore extent cache is already loaded, we'll just use the
	 * incore extent scanner to record mappings.  Don't bother walking the
	 * ondisk extent tree.
	 */
	if (!xfs_need_iread_extents(ifp))
		return 0;

	/* Accumulate all the mappings in the bmap btree. */
	cur = xfs_bmbt_init_cursor(rr->sc->mp, rr->sc->tp, ip, XFS_DATA_FORK);
	error = xfs_bmap_query_all(cur, xrep_rtrmap_visit_bmbt, rf);
	xfs_btree_del_cursor(cur, error);
	if (error)
		return error;

	/* Stash any remaining accumulated rmaps and exit. */
	*mappings_done = true;
	return xrep_rtrmap_stash_accumulated(rf);
}

/*
 * Iterate the in-core extent cache to collect rmap records for anything in
 * this fork that matches the AG.
 */
STATIC int
xrep_rtrmap_scan_iext(
	struct xrep_rtrmap_ifork *rf,
	struct xfs_ifork	*ifp)
{
	struct xfs_bmbt_irec	rec;
	struct xfs_iext_cursor	icur;
	int			error;

	for_each_xfs_iext(ifp, &icur, &rec) {
		if (isnullstartblock(rec.br_startblock))
			continue;
		error = xrep_rtrmap_visit_bmbt(NULL, &rec, rf);
		if (error)
			return error;
	}

	return xrep_rtrmap_stash_accumulated(rf);
}

/* Find all the extents on the realtime device mapped by an inode fork. */
STATIC int
xrep_rtrmap_scan_dfork(
	struct xrep_rtrmap	*rr,
	struct xfs_inode	*ip)
{
	struct xrep_rtrmap_ifork rf = {
		.accum		= { .rm_owner = ip->i_ino, },
		.rr		= rr,
	};
	struct xfs_ifork	*ifp = XFS_IFORK_PTR(ip, XFS_DATA_FORK);
	int			error = 0;

	if (ifp->if_format == XFS_DINODE_FMT_BTREE) {
		bool		mappings_done;

		/*
		 * Scan the bmbt for mappings.  If the incore extent tree is
		 * loaded, we want to scan the cached mappings since that's
		 * faster when the extent counts are very high.
		 */
		error = xrep_rtrmap_scan_bmbt(&rf, ip, &mappings_done);
		if (error || mappings_done)
			return error;
	} else if (ifp->if_format != XFS_DINODE_FMT_EXTENTS) {
		/* realtime data forks should only be extents or btree */
		return -EFSCORRUPTED;
	}

	/* Scan incore extent cache. */
	return xrep_rtrmap_scan_iext(&rf, ifp);
}

/* Record reverse mappings for a file. */
STATIC int
xrep_rtrmap_scan_inode(
	struct xrep_rtrmap	*rr,
	struct xfs_inode	*ip)
{
	unsigned int		lock_mode;
	int			error = 0;

	xfs_ilock(ip, XFS_IOLOCK_SHARED | XFS_MMAPLOCK_SHARED);
	lock_mode = xfs_ilock_data_map_shared(ip);

	/* Check the data fork if it's on the realtime device. */
	if (XFS_IS_REALTIME_INODE(ip)) {
		error = xrep_rtrmap_scan_dfork(rr, ip);
		if (error)
			goto out_unlock;
	}

	xchk_iscan_mark_visited(&rr->iscan, ip);
out_unlock:
	xfs_iunlock(ip, XFS_IOLOCK_SHARED | XFS_MMAPLOCK_SHARED | lock_mode);
	return error;
}

/* Record extents that belong to the realtime rmap inode. */
STATIC int
xrep_rtrmap_walk_rmap(
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec,
	void				*priv)
{
	struct xrep_rtrmap		*rr = priv;
	struct xfs_mount		*mp = cur->bc_mp;
	xfs_fsblock_t			fsbno;
	int				error = 0;

	if (xchk_should_terminate(rr->sc, &error))
		return error;

	/* Skip extents which are not owned by this inode and fork. */
	if (rec->rm_owner != mp->m_rrmapip->i_ino)
		return 0;

	error = xrep_check_ino_btree_mapping(rr->sc, rec);
	if (error)
		return error;

	fsbno = XFS_AGB_TO_FSB(mp, cur->bc_ag.pag->pag_agno,
			rec->rm_startblock);

	return xbitmap_set(&rr->old_rtrmapbt_blocks, fsbno, rec->rm_blockcount);
}

/* Scan one AG for reverse mappings for the realtime rmap btree. */
STATIC int
xrep_rtrmap_scan_ag(
	struct xrep_rtrmap	*rr,
	struct xfs_perag	*pag)
{
	struct xfs_scrub	*sc = rr->sc;
	int			error;

	error = xrep_ag_init(sc, pag, &sc->sa);
	if (error)
		return error;

	error = xfs_rmap_query_all(sc->sa.rmap_cur, xrep_rtrmap_walk_rmap, rr);
	xchk_ag_free(sc, &sc->sa);
	return error;
}

/* Count and check all collected records. */
STATIC int
xrep_rtrmap_check_record(
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec,
	void				*priv)
{
	struct xrep_rtrmap		*rr = priv;
	int				error;

	error = xrep_rtrmap_check_mapping(rr->sc, rec);
	if (error)
		return error;

	rr->nr_records++;
	return 0;
}

/* Generate all the reverse-mappings for the realtime device. */
STATIC int
xrep_rtrmap_find_rmaps(
	struct xrep_rtrmap	*rr)
{
	struct xfs_scrub	*sc = rr->sc;
	struct xchk_iscan	*iscan = &rr->iscan;
	struct xfs_perag	*pag;
	struct xfs_buf		*mhead_bp;
	struct xfs_btree_cur	*mcur;
	xfs_agnumber_t		agno;
	int			error;

	/*
	 * Set up for a potentially lengthy filesystem scan by reducing our
	 * transaction resource usage for the duration.  Specifically:
	 *
	 * Unlock the realtime metadata inodes and cancel the transaction to
	 * release the log grant space while we scan the filesystem.
	 *
	 * Create a new empty transaction to eliminate the possibility of the
	 * inode scan deadlocking on cyclical metadata.
	 *
	 * We pass the empty transaction to the file scanning function to avoid
	 * repeatedly cycling empty transactions.  This can be done even though
	 * we take the IOLOCK to quiesce the file because empty transactions
	 * do not take sb_internal.
	 */
	xchk_trans_cancel(sc);
	xchk_rt_unlock(sc, &sc->sr);
	error = xchk_trans_alloc_empty(sc);
	if (error)
		return error;

	while ((error = xchk_iscan_advance(sc, iscan)) == 1) {
		struct xfs_inode	*ip;

		if (xrep_is_rtmeta_ino(sc, iscan->cursor_ino))
			continue;

		error = xchk_iscan_iget(sc, iscan, &ip);
		if (error == -EAGAIN)
			continue;
		if (error)
			break;

		error = xrep_rtrmap_scan_inode(rr, ip);
		xchk_irele(sc, ip);
		if (error)
			break;

		if (xchk_should_terminate(sc, &error))
			break;
	}
	if (error)
		return error;

	/*
	 * Switch out for a real transaction and lock the RT metadata in
	 * preparation for building a new tree.
	 */
	xchk_trans_cancel(sc);
	error = xchk_setup_fs(sc);
	if (error)
		return error;
	error = xchk_rt_lock(sc, &sc->sr);
	if (error)
		return error;

	/*
	 * If a hook failed to update the in-memory btree, we lack the data to
	 * continue the repair.
	 */
	if (xchk_iscan_aborted(&rr->iscan))
		return -EFSCORRUPTED;

	/* Scan for old rtrmap blocks. */
	for_each_perag(sc->mp, agno, pag) {
		error = xrep_rtrmap_scan_ag(rr, pag);
		if (error) {
			xfs_perag_put(pag);
			return error;
		}
	}

	/*
	 * Now that we have everything locked again, we need to count the
	 * number of rmap records stashed in the btree.  This should reflect
	 * all actively-owned rt files in the filesystem.  At the same time,
	 * check all our records before we start building a new btree, which
	 * requires the rtbitmap lock.
	 */
	error = xfbtree_head_read_buf(rr->rtrmap_btree, NULL, &mhead_bp);
	if (error)
		return error;

	mcur = xfs_rtrmapbt_mem_cursor(rr->sc->mp, NULL, mhead_bp,
			rr->rtrmap_btree);
	rr->nr_records = 0;
	error = xfs_rmap_query_all(mcur, xrep_rtrmap_check_record, rr);
	xfs_btree_del_cursor(mcur, error);
	xfs_buf_relse(mhead_bp);

	return error;
}

/* Building the new rtrmap btree. */

/* Retrieve rtrmapbt data for bulk load. */
STATIC int
xrep_rtrmap_get_record(
	struct xfs_btree_cur	*cur,
	void			*priv)
{
	struct xrep_rtrmap	*rr = priv;
	int			stat = 0;
	int			error;

	error = xfs_btree_increment(rr->mcur, 0, &stat);
	if (error)
		return error;
	if (!stat)
		return -EFSCORRUPTED;

	error = xfs_rmap_get_rec(rr->mcur, &cur->bc_rec.r, &stat);
	if (error)
		return error;
	if (!stat)
		return -EFSCORRUPTED;

	return 0;
}

/* Feed one of the new btree blocks to the bulk loader. */
STATIC int
xrep_rtrmap_claim_block(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	void			*priv)
{
	struct xrep_rtrmap        *rr = priv;

	return xrep_newbt_claim_block(cur, &rr->new_btree_info, ptr);
}

/* Figure out how much space we need to create the incore btree root block. */
STATIC size_t
xrep_rtrmap_iroot_size(
	struct xfs_btree_cur	*cur,
	unsigned int		level,
	unsigned int		nr_this_level,
	void			*priv)
{
	return xfs_rtrmap_broot_space_calc(cur->bc_mp, level, nr_this_level);
}

/*
 * Use the collected rmap information to stage a new rmap btree.  If this is
 * successful we'll return with the new btree root information logged to the
 * repair transaction but not yet committed.  This implements section (III)
 * above.
 */
STATIC int
xrep_rtrmap_build_new_tree(
	struct xrep_rtrmap	*rr)
{
	struct xfs_owner_info	oinfo;
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_btree_cur	*cur;
	struct xfs_buf		*mhead_bp;
	int			error;

	rr->rtrmap_bload.get_record = xrep_rtrmap_get_record;
	rr->rtrmap_bload.claim_block = xrep_rtrmap_claim_block;
	rr->rtrmap_bload.iroot_size = xrep_rtrmap_iroot_size;
	xrep_bload_estimate_slack(sc, &rr->rtrmap_bload);

	/*
	 * Prepare to construct the new btree by reserving disk space for the
	 * new btree and setting up all the accounting information we'll need
	 * to root the new btree while it's under construction and before we
	 * attach it to the realtime rmapbt inode.
	 */
	xfs_rmap_ino_bmbt_owner(&oinfo, mp->m_rrmapip->i_ino, XFS_DATA_FORK);
	xrep_newbt_init_inode(&rr->new_btree_info, sc, XFS_DATA_FORK, &oinfo);
	cur = xfs_rtrmapbt_stage_cursor(sc->mp, mp->m_rrmapip,
			&rr->new_btree_info.ifake);

	/* Compute how many blocks we'll need for the rmaps collected. */
	error = xfs_btree_bload_compute_geometry(cur, &rr->rtrmap_bload,
			rr->nr_records);
	if (error)
		goto err_cur;

	/* Last chance to abort before we start committing fixes. */
	if (xchk_should_terminate(sc, &error))
		goto err_cur;

	/*
	 * Guess how many blocks we're going to need to rebuild an entire
	 * rtrmapbt from the number of extents we found, and pump up our
	 * transaction to have sufficient block reservation.
	 */
	error = xfs_trans_reserve_more_inode(sc->tp, mp->m_rrmapip,
			rr->rtrmap_bload.nr_blocks, 0);
	if (error)
		goto err_cur;

	/* Reserve the space we'll need for the new btree. */
	error = xrep_newbt_alloc_blocks(&rr->new_btree_info,
			rr->rtrmap_bload.nr_blocks);
	if (error)
		goto err_cur;

	/*
	 * Create a cursor to the in-memory btree so that we can bulk load the
	 * new btree.
	 */
	error = xfbtree_head_read_buf(rr->rtrmap_btree, NULL, &mhead_bp);
	if (error)
		goto err_cur;

	rr->mcur = xfs_rtrmapbt_mem_cursor(mp, NULL, mhead_bp,
			rr->rtrmap_btree);
	error = xfs_btree_goto_left_edge(rr->mcur);
	if (error)
		goto err_mcur;

	/* Add all observed rmap records. */
	rr->new_btree_info.ifake.if_fork->if_format = XFS_DINODE_FMT_RMAP;
	error = xfs_btree_bload(cur, &rr->rtrmap_bload, rr);
	if (error)
		goto err_mcur;

	/*
	 * Install the new rtrmap btree in the inode.  After this point the old
	 * btree is no longer accessible, the new tree is live, and we can
	 * delete the cursor.
	 */
	xfs_rtrmapbt_commit_staged_btree(cur, sc->tp);
	xrep_inode_set_nblocks(rr->sc, rr->new_btree_info.ifake.if_blocks);
	xfs_btree_del_cursor(cur, 0);
	xfs_btree_del_cursor(rr->mcur, 0);
	rr->mcur = NULL;
	xfs_buf_relse(mhead_bp);

	/*
	 * Now that we've written the new btree to disk, we don't need to keep
	 * updating the in-memory btree.  Abort the scan to stop live updates.
	 */
	xchk_iscan_abort(&rr->iscan);

	/* Dispose of any unused blocks and the accounting information. */
	xrep_newbt_destroy(&rr->new_btree_info, error);

	return xrep_roll_trans(sc);

err_mcur:
	xfs_btree_del_cursor(rr->mcur, error);
	xfs_buf_relse(mhead_bp);
err_cur:
	xfs_btree_del_cursor(cur, error);
	xrep_newbt_destroy(&rr->new_btree_info, error);
	return error;
}

/* Reaping the old btree. */

/* Reap the old rtrmapbt blocks. */
STATIC int
xrep_rtrmap_remove_old_tree(
	struct xrep_rtrmap	*rr)
{
	struct xfs_owner_info	oinfo;
	int			error;

	xfs_rmap_ino_bmbt_owner(&oinfo, rr->sc->ip->i_ino, XFS_DATA_FORK);

	/*
	 * Free all the extents that were allocated to the former rtrmapbt and
	 * aren't cross-linked with something else.
	 */
	error = xrep_reap_extents(rr->sc, &rr->old_rtrmapbt_blocks, &oinfo,
			XFS_AG_RESV_IMETA);
	if (error)
		return error;

	/*
	 * Ensure the proper reservation for the rtrmap inode so that we don't
	 * fail to expand the new btree.
	 */
	return xrep_reset_imeta_reservation(rr->sc);
}

static inline bool
xrep_rtrmapbt_want_live_update(
	struct xchk_iscan		*iscan,
	const struct xfs_owner_info	*oi)
{
	if (xchk_iscan_aborted(iscan))
		return false;

	/*
	 * We scanned the CoW staging extents before we started the iscan, so
	 * we need all the updates.
	 */
	if (XFS_RMAP_NON_INODE_OWNER(oi->oi_owner))
		return true;

	/* Ignore updates to files that the scanner hasn't visited yet. */
	return xchk_iscan_want_live_update(iscan, oi->oi_owner);
}

/*
 * Apply a rtrmapbt update from the regular filesystem into our shadow btree.
 * We're running from the thread that owns the rtrmap ILOCK and is generating
 * the update, so we must be careful about which parts of the struct
 * xrep_rtrmap that we change.
 */
static int
xrep_rtrmapbt_live_update(
	struct notifier_block		*nb,
	unsigned long			op,
	void				*data)
{
	struct xfs_rmap_update_params	*p = data;
	struct xrep_rtrmap		*rr;
	struct xfs_mount		*mp;
	struct xfs_btree_cur		*mcur;
	struct xfs_buf			*mhead_bp;
	int				error;

	rr = container_of(nb, struct xrep_rtrmap, rtrmap_update_hook);
	mp = rr->sc->mp;

	if (!xrep_rtrmapbt_want_live_update(&rr->iscan, &p->oinfo))
		goto out_unlock;

	trace_xrep_rtrmap_live_update(mp, op, p);

	mutex_lock(&rr->lock);
	error = xfbtree_head_read_buf(rr->rtrmap_btree, p->tp, &mhead_bp);
	if (error)
		goto out_abort;

	mcur = xfs_rtrmapbt_mem_cursor(mp, p->tp, mhead_bp, rr->rtrmap_btree);
	error = __xfs_rmap_finish_intent(mcur, op, p->startblock,
			p->blockcount, &p->oinfo, p->unwritten);
	xfs_btree_del_cursor(mcur, error);
	if (error)
		goto out_cancel;

	error = xfbtree_trans_commit(rr->rtrmap_btree, p->tp);
	if (error)
		goto out_abort;

	mutex_unlock(&rr->lock);
	return NOTIFY_DONE;

out_cancel:
	xfbtree_trans_cancel(rr->rtrmap_btree, p->tp);
out_abort:
	xchk_iscan_abort(&rr->iscan);
	mutex_unlock(&rr->lock);
out_unlock:
	return NOTIFY_DONE;
}

/* Repair the realtime rmap btree. */
int
xrep_rtrmapbt(
	struct xfs_scrub	*sc)
{
	struct xrep_rtrmap	*rr;
	int			error;

	rr = kmem_zalloc(sizeof(struct xrep_rtrmap), KM_NOFS | KM_MAYFAIL);
	if (!rr)
		return -ENOMEM;
	rr->sc = sc;

	mutex_init(&rr->lock);
	xbitmap_init(&rr->old_rtrmapbt_blocks);

	/* Set up some storage */
	error = xfs_rtrmapbt_mem_create(sc->mp, sc->xfile_buftarg,
			&rr->rtrmap_btree);
	if (error)
		goto out_bitmap;

	rr->iscan.iget_tries = 20;
	rr->iscan.iget_retry_delay = HZ / 10;
	xchk_iscan_start(&rr->iscan);

	/*
	 * Hook into live rtrmap operations so that we can update our in-memory
	 * btree to reflect live changes on the filesystem.  Since we drop the
	 * rtrmap ILOCK to scan all the inodes, we need this piece to avoid
	 * installing a stale btree.
	 */
	error = xfs_hook_add(&sc->mp->m_rtrmap_update_hooks,
			&rr->rtrmap_update_hook, xrep_rtrmapbt_live_update);
	if (error)
		goto out_records;

	/* Collect rmaps for realtime files. */
	error = xrep_rtrmap_find_rmaps(rr);
	if (error)
		goto out_hook;

	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	error = xrep_ino_dqattach(sc);
	if (error)
		goto out_hook;

	/* Rebuild the rtrmap information. */
	error = xrep_rtrmap_build_new_tree(rr);
	if (error)
		goto out_hook;

	/* Kill the old tree. */
	error = xrep_rtrmap_remove_old_tree(rr);

out_hook:
	xchk_iscan_abort(&rr->iscan);
	xfs_hook_del(&sc->mp->m_rtrmap_update_hooks, &rr->rtrmap_update_hook);
out_records:
	xchk_iscan_finish(&rr->iscan);
	xfbtree_destroy(rr->rtrmap_btree);
out_bitmap:
	xbitmap_destroy(&rr->old_rtrmapbt_blocks);
	mutex_destroy(&rr->lock);
	kmem_free(rr);
	return error;
}
