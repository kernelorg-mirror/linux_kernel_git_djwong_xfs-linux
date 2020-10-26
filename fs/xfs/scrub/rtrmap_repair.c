// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
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
#include "xfs_iwalk.h"
#include "xfs_quota.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/bitmap.h"
#include "scrub/array.h"
#include "scrub/xfile.h"

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
	struct xfbma		*rtrmap_records;

	struct xfs_scrub	*sc;

	/* bitmap of old rtrmapbt blocks */
	struct xbitmap		old_rtrmapbt_blocks;

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
		.rm_offset	= offset,
		.rm_flags	= flags,
	};
	int			error = 0;

	trace_xrep_rtrmap_found(rr->sc->mp, startblock, blockcount, owner,
			offset, flags);

	if (xchk_should_terminate(rr->sc, &error))
		return error;

	rre.offset = xfs_rmap_irec_offset_pack(&rmap);
	return xfbma_append(rr->rtrmap_records, &rre);
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

	/* Transaction associated with this rmap recovery attempt. */
	struct xfs_trans	*tp;
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
 * fork that matches the AG.
 */
STATIC int
xrep_rtrmap_scan_bmbt(
	struct xrep_rtrmap_ifork *rf,
	struct xfs_inode	*ip,
	bool			*done)
{
	struct xrep_rtrmap	*rr = rf->rr;
	struct xfs_btree_cur	*cur;
	struct xfs_ifork	*ifp = XFS_IFORK_PTR(ip, XFS_DATA_FORK);
	int			error = 0;

	*done = false;

	/*
	 * If the incore extent cache is already loaded, we'll just use the
	 * incore extent scanner to record mappings.  Don't bother walking the
	 * ondisk extent tree.
	 */
	if (ifp->if_flags & XFS_IFEXTENTS)
		return 0;

	/* Accumulate all the mappings in the bmap btree. */
	cur = xfs_bmbt_init_cursor(rr->sc->mp, rf->tp, ip, XFS_DATA_FORK);
	error = xfs_bmap_query_all(cur, xrep_rtrmap_visit_bmbt, rf);
	xfs_btree_del_cursor(cur, error);
	if (error)
		return error;

	/* Stash any remaining accumulated rmaps and exit. */
	*done = true;
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
xrep_rtrmap_scan_ifork(
	struct xrep_rtrmap	*rr,
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	struct xrep_rtrmap_ifork	rf = {
		.accum		= { .rm_owner = ip->i_ino, },
		.rr		= rr,
		.tp		= tp,
	};
	struct xfs_ifork	*ifp = XFS_IFORK_PTR(ip, XFS_DATA_FORK);
	bool			done;
	int			error = 0;

	/* Do we even have data mapping extents? */
	if (!ifp)
		return 0;

	switch (ifp->if_format) {
	case XFS_DINODE_FMT_BTREE:
		error = xrep_rtrmap_scan_bmbt(&rf, ip, &done);
		if (error || done)
			return error;
		break;
	case XFS_DINODE_FMT_EXTENTS:
		break;
	default:
		return 0;
	}

	/* Scan incore extent cache. */
	return xrep_rtrmap_scan_iext(&rf, ifp);
}

/* Record reverse mappings for a file. */
STATIC int
xrep_rtrmap_scan_inode(
	struct xfs_mount		*mp,
	struct xfs_trans		*tp,
	xfs_ino_t			ino,
	void				*data)
{
	struct xrep_rtrmap		*rr = data;
	struct xfs_inode		*ip;
	int				error;

	if (ino == mp->m_rrmapip->i_ino ||
	    ino == mp->m_rrefcountip->i_ino ||
	    ino == mp->m_rbmip->i_ino ||
	    ino == mp->m_rsumip->i_ino)
		return 0;

	/* Grab inode and lock it so we can scan it. */
	error = xfs_iget(mp, rr->sc->tp, ino,
			XFS_IGET_DONTCACHE | XFS_IGET_UNLINKED, 0, &ip);
	if (error)
		return error;

	/*
	 * The fs is frozen, which means that nobody should be holding a data
	 * file's ILOCK /and/ waiting for the rt metadata inodes.  However,
	 * this is technically an ABBA deadlock vector, so we have to use the
	 * deadlock-avoidant locking routine to avoid tripping up lockdep.  We
	 * avoid modifying the inode's incore extent tree, so we can use a
	 * shared lock here.
	 */
	error = xchk_ilock_inverted(ip, XFS_ILOCK_SHARED);
	if (error)
		goto out_rele;

	if (!XFS_IS_REALTIME_INODE(ip))
		goto out_unlock;

	/* Check the data fork. */
	error = xrep_rtrmap_scan_ifork(rr, tp, ip);
	if (error)
		goto out_unlock;

out_unlock:
	xfs_iunlock(ip, XFS_ILOCK_SHARED);
out_rele:
	xfs_irele(ip);
	return error;
}

/* Record extents that belong to the realtime rmap inode. */
STATIC int
xrep_rtrmap_walk_rmap(
	struct xfs_btree_cur	*cur,
	struct xfs_rmap_irec	*rec,
	void			*priv)
{
	struct xrep_rtrmap	*rr = priv;
	struct xfs_mount	*mp = cur->bc_mp;
	xfs_fsblock_t		fsbno;
	int			error = 0;

	if (xchk_should_terminate(rr->sc, &error))
		return error;

	/* Skip extents which are not owned by this inode and fork. */
	if (rec->rm_owner != mp->m_rrmapip->i_ino)
		return 0;

	/*
	 * The realtime rmap inode should never have extended attributes, and
	 * all blocks should have the bmbt block flag set.
	 */
	if ((rec->rm_flags & XFS_RMAP_ATTR_FORK) ||
	    !(rec->rm_flags & XFS_RMAP_BMBT_BLOCK))
		return -EFSCORRUPTED;

	fsbno = XFS_AGB_TO_FSB(mp, cur->bc_ag.agno, rec->rm_startblock);

	return xbitmap_set(&rr->old_rtrmapbt_blocks, fsbno, rec->rm_blockcount);
}

/* Scan one AG for reverse mappings for the realtime rmap btree. */
STATIC int
xrep_rtrmap_scan_ag(
	struct xrep_rtrmap	*rr,
	xfs_agnumber_t		agno)
{
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_buf		*agf_bp = NULL;
	struct xfs_btree_cur	*cur;
	int			error;

	error = xfs_alloc_read_agf(mp, sc->tp, agno, 0, &agf_bp);
	if (error)
		return error;
	if (!agf_bp)
		return -ENOMEM;
	cur = xfs_rmapbt_init_cursor(mp, sc->tp, agf_bp, agno);
	error = xfs_rmap_query_all(cur, xrep_rtrmap_walk_rmap, rr);
	xfs_btree_del_cursor(cur, error);
	xfs_trans_brelse(sc->tp, agf_bp);
	return error;
}

/* Generate all the reverse-mappings for the realtime device. */
STATIC int
xrep_rtrmap_find_rmaps(
	struct xrep_rtrmap	*rr)
{
	struct xfs_scrub	*sc = rr->sc;
	xfs_agnumber_t		agno;
	int			error;

	error = xfs_iwalk(sc->mp, sc->tp, 0, 0, xrep_rtrmap_scan_inode, 0, rr);
	if (error)
		return error;

	/* Scan for old rtrmap blocks. */
	for (agno = 0; agno < sc->mp->m_sb.sb_agcount; agno++) {
		error = xrep_rtrmap_scan_ag(rr, agno);
		if (error)
			return error;
	}

	return 0;
}

/* Building the new rtrmap btree. */

/* Update the rtrmap inode counters. */
STATIC int
xrep_rtrmap_reset_counters(
	struct xrep_rtrmap	*rr)
{
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xbtree_ifakeroot	*ifake = &rr->new_btree_info.ifake;
	int64_t			delta;
	int			error;

	/*
	 * Update the inode block counts to reflect the extents we found in the
	 * rmapbt.
	 */
	delta = ifake->if_blocks - mp->m_rrmapip->i_d.di_nblocks;
	mp->m_rrmapip->i_d.di_nblocks = ifake->if_blocks;
	xfs_trans_log_inode(sc->tp, mp->m_rrmapip, XFS_ILOG_CORE);

	/*
	 * Adjust the quota counts by the difference in size between the old
	 * and new bmbt.
	 */
	if (delta == 0 || !XFS_IS_QUOTA_ON(sc->mp))
		return 0;

	error = xrep_ino_dqattach(sc);
	if (error)
		return error;

	xfs_trans_mod_dquot_byino(sc->tp, mp->m_rrmapip, XFS_TRANS_DQ_BCOUNT,
			delta);
	return 0;
}

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

	error = xfbma_iter_get(rr->rtrmap_records, &rr->iter, &rec);
	if (error)
		return error;

	irec->rm_startblock = rec.startblock;
	irec->rm_blockcount = rec.blockcount;
	irec->rm_owner = rec.owner;
	return xfs_rmap_irec_offset_unpack(rec.offset, irec);
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
	error = xfbma_sort(rr->rtrmap_records, xrep_rtrmap_extent_cmp);
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

	nr_records = xfbma_length(rr->rtrmap_records);

	/* Compute how many blocks we'll need for the rmaps collected. */
	error = xfs_btree_bload_compute_geometry(cur, &rr->rtrmap_bload,
			nr_records);
	if (error)
		goto err_cur;

	/*
	 * Guess how many blocks we're going to need to rebuild an entire
	 * rtrmapbt from the number of extents we found, and pump up our
	 * transaction to have sufficient block reservation.
	 */
	error = xfs_trans_reserve_more(sc->tp, rr->rtrmap_bload.nr_blocks, 0);
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
	 * btree is no longer accessible and the new tree is live and we can
	 * delete the cursor.
	 */
	xfs_rtrmapbt_commit_staged_btree(cur, sc->tp);
	xfs_btree_del_cursor(cur, 0);

	/* Reset the inode counters now that we've changed the btree shape. */
	error = xrep_rtrmap_reset_counters(rr);
	if (error)
		goto err_newbt;

	/* Dispose of any unused blocks and the accounting information. */
	xrep_newbt_destroy(&rr->new_btree_info, error);
	return xrep_roll_trans(sc);

err_cur:
	xfs_btree_del_cursor(cur, error);
err_newbt:
	xrep_newbt_destroy(&rr->new_btree_info, error);
	return error;
}

/* Reaping the old btree. */

/* Reap the old rtrmapbt blocks. */
STATIC int
xrep_rtrmap_remove_old_tree(
	struct xrep_rtrmap	*rr)
{
	/*
	 * Free all the extents that were allocated to the old rtrmapbt.
	 */
	return xrep_reap_extents(rr->sc, &rr->old_rtrmapbt_blocks,
			&XFS_RMAP_OINFO_ANY_OWNER, XFS_AG_RESV_RTMETADATA);
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
	rr->rtrmap_records = xfbma_init("rtrmap records",
			sizeof(struct xrep_rtrmap_extent));
	if (IS_ERR(rr->rtrmap_records)) {
		error = PTR_ERR(rr->rtrmap_records);
		goto out_bitmap;
	}

	/* Collect rmaps for realtime files. */
	error = xrep_rtrmap_find_rmaps(rr);
	if (error)
		goto out_records;

	/* Rebuild the rtrmap information. */
	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	error = xrep_rtrmap_build_new_tree(rr);
	if (error)
		goto out_records;

	/* Kill the old tree. */
	error = xrep_rtrmap_remove_old_tree(rr);

out_records:
	xfbma_destroy(rr->rtrmap_records);
out_bitmap:
	xbitmap_destroy(&rr->old_rtrmapbt_blocks);
	kmem_free(rr);
	return error;
}
