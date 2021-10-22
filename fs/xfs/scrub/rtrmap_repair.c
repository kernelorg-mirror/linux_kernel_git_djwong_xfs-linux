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
#include "xfs_defer.h"
#include "xfs_btree.h"
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
#include "xfs_refcount.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/bitmap.h"
#include "scrub/xfarray.h"
#include "scrub/xfile.h"
#include "scrub/iscan.h"

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
	/*
	 * Freeze out anything that can lock an inode.  We reconstruct the
	 * rtrmapbt by reading inode bmaps with the rt rmap btree inode locked,
	 * which is only safe w.r.t. ABBA deadlocks if we're the only ones
	 * locking inodes.
	 */
	return xchk_fs_freeze(sc);
}

/*
 * Packed rmap record.  The UNWRITTEN flags are hidden in the upper bits of
 * offset, just like the on-disk record.
 */
struct xrep_rtrmap_extent {
	xfs_rtblock_t	startblock;
	xfs_filblks_t	blockcount;
	uint64_t	owner;
	uint64_t	offset;
} __packed;

/* Context for collecting rmaps */
struct xrep_rtrmap {
	/* new rtrmapbt information */
	struct xrep_newbt	new_btree_info;
	struct xfs_btree_bload	rtrmap_bload;

	/* rmap records generated from primary metadata */
	struct xfarray		*rtrmap_records;

	struct xfs_scrub	*sc;

	/* bitmap of old rtrmapbt blocks */
	struct xbitmap		old_rtrmapbt_blocks;

	/* inode scan cursor */
	struct xchk_iscan	iscan;

	/* get_record()'s position in the free space record array. */
	uint64_t		iter;
};

/* Compare two rtrmapbt extents. */
static int
xrep_rtrmap_extent_cmp(
	const void			*a,
	const void			*b)
{
	const struct xrep_rtrmap_extent	*ap = a;
	const struct xrep_rtrmap_extent	*bp = b;
	struct xfs_rmap_irec		ar = {
		.rm_startblock		= ap->startblock,
		.rm_blockcount		= ap->blockcount,
		.rm_owner		= ap->owner,
	};
	struct xfs_rmap_irec		br = {
		.rm_startblock		= bp->startblock,
		.rm_blockcount		= bp->blockcount,
		.rm_owner		= bp->owner,
	};
	int				error;

	error = xfs_rmap_irec_offset_unpack(ap->offset, &ar);
	if (error)
		ASSERT(error == 0);

	error = xfs_rmap_irec_offset_unpack(bp->offset, &br);
	if (error)
		ASSERT(error == 0);

	return xfs_rmap_compare(&ar, &br);
}

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
			rec->rm_blockcount, false);
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
	struct xrep_rtrmap_extent	rre = {
		.startblock	= startblock,
		.blockcount	= blockcount,
		.owner		= owner,
	};
	struct xfs_rmap_irec	rmap = {
		.rm_startblock	= startblock,
		.rm_blockcount	= blockcount,
		.rm_owner	= owner,
		.rm_offset	= offset,
		.rm_flags	= flags,
	};
	struct xfs_scrub	*sc = rr->sc;
	int			error = 0;

	if (xchk_should_terminate(sc, &error))
		return error;

	trace_xrep_rtrmap_found(sc->mp, &rmap);

	rre.offset = xfs_rmap_irec_offset_pack(&rmap);
	return xfarray_append(rr->rtrmap_records, &rre);
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

struct xrep_rtrmap_stash_run {
	struct xrep_rtrmap	*rr;
	uint64_t		owner;
};

static int
xrep_rtrmap_stash_run(
	uint64_t			start,
	uint64_t			len,
	void				*priv)
{
	struct xrep_rtrmap_stash_run	*rsr = priv;
	struct xrep_rtrmap		*rr = rsr->rr;

	return xrep_rtrmap_stash(rr, start, len, rsr->owner, 0, 0);
}

/*
 * Emit rmaps for every extent of bits set in the bitmap.  Caller must ensure
 * that the ranges are in units of FS blocks.
 */
STATIC int
xrep_rtrmap_stash_bitmap(
	struct xrep_rtrmap		*rr,
	struct xbitmap			*bitmap,
	const struct xfs_owner_info	*oinfo)
{
	struct xrep_rtrmap_stash_run	rsr = {
		.rr			= rr,
		.owner			= oinfo->oi_owner,
	};

	return xbitmap_walk(bitmap, xrep_rtrmap_stash_run, &rsr);
}

/* Record a CoW staging extent. */
STATIC int
xrep_rtrmap_walk_cowblocks(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_rec	*rec,
	void				*priv)
{
	struct xbitmap			*bitmap = priv;
	struct xfs_refcount_irec	refc;
	xfs_fsblock_t			fsbno;

	xfs_refcount_btrec_to_irec(cur, rec, &refc);
	if (refc.rc_refcount != 1)
		return -EFSCORRUPTED;

	fsbno = refc.rc_startblock - XFS_RTREFC_COW_START;
	return xbitmap_set(bitmap, fsbno, refc.rc_blockcount);
}

/*
 * Collect rmaps for the blocks containing the refcount btree, and all CoW
 * staging extents.
 */
STATIC int
xrep_rtrmap_find_refcount_rmaps(
	struct xrep_rtrmap	*rr)
{
	struct xbitmap		cow_blocks;		/* COWBIT */
	union xfs_btree_irec	low;
	union xfs_btree_irec	high;
	struct xfs_scrub	*sc = rr->sc;
	int			error;

	if (!xfs_has_rtreflink(sc->mp))
		return 0;

	xbitmap_init(&cow_blocks);

	/* Collect rmaps for CoW staging extents. */
	memset(&low, 0, sizeof(low));
	low.rc.rc_startblock = XFS_RTREFC_COW_START;
	memset(&high, 0xFF, sizeof(high));
	error = xfs_btree_query_range(sc->sr.refc_cur, &low, &high,
			xrep_rtrmap_walk_cowblocks, &cow_blocks);
	if (error)
		goto out_bitmap;

	/* Generate rmaps for everything. */
	error = xrep_rtrmap_stash_bitmap(rr, &cow_blocks, &XFS_RMAP_OINFO_COW);
	if (error)
		goto out_bitmap;

out_bitmap:
	xbitmap_destroy(&cow_blocks);
	return error;
}

/* Generate all the reverse-mappings for the realtime device. */
STATIC int
xrep_rtrmap_find_rmaps(
	struct xrep_rtrmap	*rr)
{
	struct xfs_scrub	*sc = rr->sc;
	struct xchk_iscan	*iscan = &rr->iscan;
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno;
	int			error;

	/* Find CoW staging extents. */
	xrep_rt_btcur_init(sc, &sc->sr);
	error = xrep_rtrmap_find_refcount_rmaps(rr);
	xchk_rt_btcur_free(&sc->sr);
	if (error)
		return error;

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

		if (xrep_is_rtmeta_ino(rr->sc, iscan->cursor_ino))
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

	/* Scan for old rtrmap blocks. */
	for_each_perag(sc->mp, agno, pag) {
		error = xrep_rtrmap_scan_ag(rr, pag);
		if (error) {
			xfs_perag_put(pag);
			return error;
		}
	}

	return 0;
}

/* Building the new rtrmap btree. */

/* Retrieve rtrmapbt data for bulk load. */
STATIC int
xrep_rtrmap_get_record(
	struct xfs_btree_cur	*cur,
	void			*priv)
{
	struct xrep_rtrmap_extent	rec;
	struct xfs_rmap_irec	*irec = &cur->bc_rec.r;
	struct xrep_rtrmap	*rr = priv;
	int			error;

	error = xfarray_load_next(rr->rtrmap_records, &rr->iter, &rec);
	if (error)
		return error;

	irec->rm_startblock = rec.startblock;
	irec->rm_blockcount = rec.blockcount;
	irec->rm_owner = rec.owner;

	error = xfs_rmap_irec_offset_unpack(rec.offset, irec);
	if (error)
		return error;

	return xrep_rtrmap_check_mapping(rr->sc, irec);
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
	uint64_t		nr_records;
	int			error;

	rr->rtrmap_bload.get_record = xrep_rtrmap_get_record;
	rr->rtrmap_bload.claim_block = xrep_rtrmap_claim_block;
	rr->rtrmap_bload.iroot_size = xrep_rtrmap_iroot_size;
	xrep_bload_estimate_slack(sc, &rr->rtrmap_bload);

	/*
	 * Sort the rmap records by startblock or else the btree records
	 * will be in the wrong order.
	 */
	error = xfarray_sort(rr->rtrmap_records, xrep_rtrmap_extent_cmp);
	if (error)
		return error;

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

	nr_records = xfarray_length(rr->rtrmap_records);

	/* Compute how many blocks we'll need for the rmaps collected. */
	error = xfs_btree_bload_compute_geometry(cur, &rr->rtrmap_bload,
			nr_records);
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

	/* Add all observed rmap records. */
	rr->new_btree_info.ifake.if_fork->if_format = XFS_DINODE_FMT_RMAP;
	rr->iter = 0;
	error = xfs_btree_bload(cur, &rr->rtrmap_bload, rr);
	if (error)
		goto err_cur;

	/*
	 * Install the new rtrmap btree in the inode.  After this point the old
	 * btree is no longer accessible, the new tree is live, and we can
	 * delete the cursor.
	 */
	xfs_rtrmapbt_commit_staged_btree(cur, sc->tp);
	xrep_inode_set_nblocks(rr->sc, rr->new_btree_info.ifake.if_blocks);
	xfs_btree_del_cursor(cur, 0);

	/* Dispose of any unused blocks and the accounting information. */
	xrep_newbt_destroy(&rr->new_btree_info, error);
	return xrep_roll_trans(sc);

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

	xbitmap_init(&rr->old_rtrmapbt_blocks);

	/* Set up some storage */
	error = xfarray_create(sc->mp, "rtrmap records",
			sizeof(struct xrep_rtrmap_extent), &rr->rtrmap_records);
	if (error)
		goto out_bitmap;

	rr->iscan.iget_tries = 20;
	rr->iscan.iget_retry_delay = HZ / 10;
	xchk_iscan_start(&rr->iscan);

	/* Collect rmaps for realtime files. */
	error = xrep_rtrmap_find_rmaps(rr);
	if (error)
		goto out_records;

	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	error = xrep_ino_dqattach(sc);
	if (error)
		goto out_records;

	/* Rebuild the rtrmap information. */
	error = xrep_rtrmap_build_new_tree(rr);
	if (error)
		goto out_records;

	/* Kill the old tree. */
	error = xrep_rtrmap_remove_old_tree(rr);

out_records:
	xchk_iscan_finish(&rr->iscan);
	xfarray_destroy(rr->rtrmap_records);
out_bitmap:
	xbitmap_destroy(&rr->old_rtrmapbt_blocks);
	kmem_free(rr);
	return error;
}
