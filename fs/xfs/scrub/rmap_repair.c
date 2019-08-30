// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
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
#include "xfs_alloc_btree.h"
#include "xfs_ialloc.h"
#include "xfs_ialloc_btree.h"
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_refcount.h"
#include "xfs_refcount_btree.h"
#include "xfs_iwalk.h"
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
 * Reverse Mapping Btree Repair
 * ============================
 *
 * This is the most involved of all the AG space btree rebuilds.  Everywhere
 * else in XFS we lock inodes and then AG data structures, but generating the
 * list of rmap records requires that we be able to scan both block mapping
 * btrees of every inode in the filesystem to see if it owns any extents in
 * this AG.  We can't tolerate any inode updates while we do this, so we
 * freeze the filesystem to lock everyone else out, and grant ourselves
 * special privileges to run transactions with regular background reclamation
 * turned off.
 *
 * We also have to be very careful not to allow inode reclaim to start a
 * transaction because all transactions (other than our own) will block.
 * Deferred inode inactivation helps us out there.
 *
 * I) Reverse mappings for all non-space metadata and file data are collected
 * according to the following algorithm:
 *
 * 1. For each fork of each inode:
 * 1.1. Create a bitmap BMBIT to track bmbt blocks if necessary.
 * 1.2. If the incore extent map isn't loaded, walk the bmbt to accumulate
 *      bmaps into rmap records (see 1.1.4).  Set bits in BMBIT for each btree
 *      block.
 * 1.3. If the incore extent map is loaded but the fork is in btree format,
 *      just visit the bmbt blocks to set the corresponding BMBIT areas.
 * 1.4. From the incore extent map, accumulate each bmap that falls into our
 *      target AG.  Remember, multiple bmap records can map to a single rmap
 *      record, so we cannot simply emit rmap records 1:1.
 * 1.5. Emit rmap records for each extent in BMBIT and free it.
 * 2. Create bitmaps INOBIT and ICHUNKBIT.
 * 3. For each record in the inobt, set the corresponding areas in ICHUNKBIT,
 *    and set bits in INOBIT for each btree block.  If the inobt has no records
 *    at all, we must be careful to record its root in INOBIT.
 * 4. For each block in the finobt, set the corresponding INOBIT area.
 * 5. Emit rmap records for each extent in INOBIT and ICHUNKBIT and free them.
 * 6. Create bitmaps REFCBIT and COWBIT.
 * 7. For each CoW staging extent in the refcountbt, set the corresponding
 *    areas in COWBIT.
 * 8. For each block in the refcountbt, set the corresponding REFCBIT area.
 * 9. Emit rmap records for each extent in REFCBIT and COWBIT and free them.
 * A. Emit rmap for the AG headers.
 * B. Emit rmap for the log, if there is one.
 *
 * II) The rmapbt shape and space metadata rmaps are computed as follows:
 *
 * 1. Count the rmaps collected in the previous step. (= NR)
 * 2. Estimate the number of rmapbt blocks needed to store NR records. (= RMB)
 * 3. Reserve RMB blocks through the newbt using the allocator in normap mode.
 * 4. Create bitmap AGBIT.
 * 5. For each reservation in the newbt, set the corresponding areas in AGBIT.
 * 6. For each block in the AGFL, bnobt, and cntbt, set the bits in AGBIT.
 * 7. Count the extents in AGBIT. (= AGNR)
 * 8. Estimate the number of rmapbt blocks needed for NR + AGNR rmaps. (= RMB')
 * 9. If RMB' >= RMB, reserve RMB' - RMB more newbt blocks, set RMB = RMB',
 *    and clear AGBIT.  Go to step 5.
 * A. Emit rmaps for each extent in AGBIT.
 *
 * III) The rmapbt is constructed and set in place as follows:
 *
 * 1. Sort the rmap records.
 * 2. Bulk load the rmaps.
 *
 * IV) Reap the old btree blocks.
 *
 * 1. Create a bitmap OLDRMBIT.
 * 2. For each gap in the new rmapbt, set the corresponding areas of OLDRMBIT.
 * 3. For each extent in the bnobt, clear the corresponding parts of OLDRMBIT.
 * 4. Reap the extents corresponding to the set areas in OLDRMBIT.  These are
 *    the parts of the AG that the rmap didn't find during its scan of the
 *    primary metadata and aren't known to be in the free space, which implies
 *    that they were the old rmapbt blocks.
 * 5. Commit.
 *
 * We use the 'xrep_rmap' prefix for all the rmap functions.
 */

/* Set us up to repair reverse mapping btrees. */
int
xrep_rmapbt_setup(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	int			error;

	/*
	 * Freeze out anything that can lock an inode.  We reconstruct
	 * the rmapbt by reading inode bmaps with the AGF held, which is
	 * only safe w.r.t. ABBA deadlocks if we're the only ones locking
	 * inodes.
	 */
	error = xchk_fs_freeze(sc);
	if (error)
		return error;

	/* Check the AG number and set up the scrub context. */
	error = xchk_setup_fs(sc, ip);
	if (error)
		return error;

	return xchk_ag_init(sc, sc->sm->sm_agno, &sc->sa);
}

/*
 * Packed rmap record.  The ATTR/BMBT/UNWRITTEN flags are hidden in the upper
 * bits of offset, just like the on-disk record.
 */
struct xrep_rmap_extent {
	xfs_agblock_t	startblock;
	xfs_extlen_t	blockcount;
	uint64_t	owner;
	uint64_t	offset;
} __packed;

/* Context for collecting rmaps */
struct xrep_rmap {
	/* new rmapbt information */
	struct xrep_newbt	new_btree_info;

	/* rmap records generated from primary metadata */
	struct xfbma		*rmap_records;

	struct xfs_scrub	*sc;

	/* get_data()'s position in the free space record array. */
	uint64_t		iter;

	/* bnobt/cntbt contribution to btreeblks */
	xfs_agblock_t		freesp_btblocks;
};

/* Compare two rmapbt extents. */
static int
xrep_rmap_extent_cmp(
	const void			*a,
	const void			*b)
{
	const struct xrep_rmap_extent	*ap = a;
	const struct xrep_rmap_extent	*bp = b;
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
	ASSERT(error == 0);
	error = xfs_rmap_irec_offset_unpack(bp->offset, &br);
	ASSERT(error == 0);

	return xfs_rmap_compare(&ar, &br);
}

/* Store a reverse-mapping record. */
static inline int
xrep_rmap_stash(
	struct xrep_rmap	*rr,
	xfs_agblock_t		startblock,
	xfs_extlen_t		blockcount,
	uint64_t		owner,
	uint64_t		offset,
	unsigned int		flags)
{
	struct xrep_rmap_extent	rre = {
		.startblock	= startblock,
		.blockcount	= blockcount,
		.owner		= owner,
	};
	struct xfs_rmap_irec	rmap = {
		.rm_offset	= offset,
		.rm_flags	= flags,
	};
	int			error = 0;

	trace_xrep_rmap_found(rr->sc->mp, rr->sc->sa.agno, startblock,
			blockcount, owner, offset, flags);

	if (xchk_should_terminate(rr->sc, &error))
		return error;

	rre.offset = xfs_rmap_irec_offset_pack(&rmap);
	return xfbma_append(rr->rmap_records, &rre);
}

struct xrep_rmap_stash_run {
	struct xrep_rmap	*rr;
	uint64_t		owner;
	unsigned int		rmap_flags;
};

static int
xrep_rmap_stash_run(
	uint64_t			start,
	uint64_t			len,
	void				*priv)
{
	struct xrep_rmap_stash_run	*rsr = priv;
	struct xrep_rmap		*rr = rsr->rr;

	return xrep_rmap_stash(rr, XFS_FSB_TO_AGBNO(rr->sc->mp, start), len,
			rsr->owner, 0, rsr->rmap_flags);
}

/*
 * Emit rmaps for every extent of bits set in the bitmap.  Caller must ensure
 * that the ranges are in units of FS blocks.
 */
STATIC int
xrep_rmap_stash_bitmap(
	struct xrep_rmap		*rr,
	struct xbitmap			*bitmap,
	const struct xfs_owner_info	*oinfo)
{
	struct xrep_rmap_stash_run	rsr = {
		.rr			= rr,
		.owner			= oinfo->oi_owner,
		.rmap_flags		= 0,
	};

	if (oinfo->oi_flags & XFS_OWNER_INFO_ATTR_FORK)
		rsr.rmap_flags |= XFS_RMAP_ATTR_FORK;
	if (oinfo->oi_flags & XFS_OWNER_INFO_BMBT_BLOCK)
		rsr.rmap_flags |= XFS_RMAP_BMBT_BLOCK;

	return xbitmap_iter_set(bitmap, xrep_rmap_stash_run, &rsr);
}

/* Section (I): Finding all file and bmbt extents. */

/* Context for accumulating rmaps for an inode fork. */
struct xrep_rmap_ifork {
	/*
	 * Accumulate rmap data here to turn multiple adjacent bmaps into a
	 * single rmap.
	 */
	struct xfs_rmap_irec	accum;

	/* Bitmap of bmbt blocks. */
	struct xbitmap		bmbt_blocks;

	struct xrep_rmap	*rr;

	/* Transaction associated with this rmap recovery attempt. */
	struct xfs_trans	*tp;

	/* Which inode fork? */
	int			whichfork;
};

/* Add a bmbt block to the bitmap. */
STATIC int
xrep_rmap_visit_bmbt_block(
	struct xfs_btree_cur	*cur,
	int			level,
	void			*priv)
{
	struct xrep_rmap_ifork	*rf = priv;
	struct xfs_buf		*bp;
	xfs_fsblock_t		fsb;

	xfs_btree_get_block(cur, level, &bp);
	if (!bp)
		return 0;

	fsb = XFS_DADDR_TO_FSB(cur->bc_mp, bp->b_bn);
	if (XFS_FSB_TO_AGNO(cur->bc_mp, fsb) != rf->rr->sc->sa.agno)
		return 0;

	return xbitmap_set(&rf->bmbt_blocks, fsb, 1);
}

/* Stash an rmap that we accumulated while walking an inode fork. */
STATIC int
xrep_rmap_stash_accumulated(
	struct xrep_rmap_ifork	*rf)
{
	if (rf->accum.rm_blockcount == 0)
		return 0;

	return xrep_rmap_stash(rf->rr, rf->accum.rm_startblock,
			rf->accum.rm_blockcount, rf->accum.rm_owner,
			rf->accum.rm_offset, rf->accum.rm_flags);
}

/* Accumulate a bmbt record. */
STATIC int
xrep_rmap_visit_bmbt(
	struct xfs_btree_cur	*cur,
	struct xfs_bmbt_irec	*rec,
	void			*priv)
{
	struct xrep_rmap_ifork	*rf = priv;
	struct xfs_mount	*mp = rf->rr->sc->mp;
	struct xfs_rmap_irec	*accum = &rf->accum;
	xfs_agblock_t		agbno;
	unsigned int		rmap_flags = 0;
	int			error;

	if (XFS_FSB_TO_AGNO(mp, rec->br_startblock) != rf->rr->sc->sa.agno)
		return 0;

	agbno = XFS_FSB_TO_AGBNO(mp, rec->br_startblock);
	if (rf->whichfork == XFS_ATTR_FORK)
		rmap_flags |= XFS_RMAP_ATTR_FORK;
	if (rec->br_state == XFS_EXT_UNWRITTEN)
		rmap_flags |= XFS_RMAP_UNWRITTEN;

	/* If this bmap is adjacent to the previous one, just add it. */
	if (accum->rm_blockcount > 0 &&
	    rec->br_startoff == accum->rm_offset + accum->rm_blockcount &&
	    agbno == accum->rm_startblock + accum->rm_blockcount &&
	    rmap_flags == accum->rm_flags) {
		accum->rm_blockcount += rec->br_blockcount;
		return 0;
	}

	/* Otherwise stash the old rmap and start accumulating a new one. */
	error = xrep_rmap_stash_accumulated(rf);
	if (error)
		return error;

	accum->rm_startblock = agbno;
	accum->rm_blockcount = rec->br_blockcount;
	accum->rm_offset = rec->br_startoff;
	accum->rm_flags = rmap_flags;
	return 0;
}

static inline bool
is_rt_data_fork(
	struct xfs_inode	*ip,
	int			whichfork)
{
	return whichfork == XFS_DATA_FORK && XFS_IS_REALTIME_INODE(ip);
}

/*
 * Iterate the block mapping btree to collect rmap records for anything in this
 * fork that matches the AG.
 */
STATIC int
xrep_rmap_scan_bmbt(
	struct xrep_rmap_ifork	*rf,
	struct xfs_inode	*ip,
	bool			*done)
{
	struct xfs_owner_info	oinfo;
	struct xrep_rmap	*rr = rf->rr;
	struct xfs_btree_cur	*cur;
	struct xfs_ifork	*ifp;
	int			error;
	bool			iterate_bmbt = false;

	*done = false;
	ifp = XFS_IFORK_PTR(ip, rf->whichfork);

	/*
	 * If the incore extent cache isn't loaded (and this isn't the data
	 * fork of a realtime inode), we only need to scan the bmbt for
	 * mapping records.  Avoid loading the cache, which will increase
	 * memory pressure at a time when we're trying to run as quickly as
	 * we possibly can.
	 */
	if (!(ifp->if_flags & XFS_IFEXTENTS) &&
	    !is_rt_data_fork(ip, rf->whichfork))
		iterate_bmbt = true;

	xbitmap_init(&rf->bmbt_blocks);
	cur = xfs_bmbt_init_cursor(rr->sc->mp, rf->tp, ip, rf->whichfork);

	/* Accumulate all the mappings in the bmap btree. */
	if (iterate_bmbt) {
		error = xfs_bmap_query_all(cur, xrep_rmap_visit_bmbt, rf);
		if (error)
			goto out_cur;
	}

	/* Record all the blocks in the bmbt itself. */
	error = xfs_btree_visit_blocks(cur, xrep_rmap_visit_bmbt_block, rf);
	if (error)
		goto out_cur;
	xfs_btree_del_cursor(cur, error);

	/* Emit rmaps for the bmbt blocks. */
	xfs_rmap_ino_bmbt_owner(&oinfo, rf->accum.rm_owner, rf->whichfork);
	error = xrep_rmap_stash_bitmap(rr, &rf->bmbt_blocks, &oinfo);
	if (error)
		goto out_bitmap;
	xbitmap_destroy(&rf->bmbt_blocks);

	/* We're done if we scanned the bmbt or it's a realtime inode. */
	*done = iterate_bmbt;

	/* Stash any remaining accumulated rmap. */
	return xrep_rmap_stash_accumulated(rf);
out_cur:
	xfs_btree_del_cursor(cur, error);
out_bitmap:
	xbitmap_destroy(&rf->bmbt_blocks);
	return error;
}

/*
 * Iterate the in-core extent cache to collect rmap records for anything in
 * this fork that matches the AG.
 */
STATIC int
xrep_rmap_scan_iext(
	struct xrep_rmap_ifork	*rf,
	struct xfs_ifork	*ifp)
{
	struct xfs_bmbt_irec	rec;
	struct xfs_iext_cursor	icur;
	int			error;

	for_each_xfs_iext(ifp, &icur, &rec) {
		if (isnullstartblock(rec.br_startblock))
			continue;
		error = xrep_rmap_visit_bmbt(NULL, &rec, rf);
		if (error)
			return error;
	}

	return xrep_rmap_stash_accumulated(rf);
}

/* Find all the extents from a given AG in an inode fork. */
STATIC int
xrep_rmap_scan_ifork(
	struct xrep_rmap	*rr,
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			whichfork)
{
	struct xrep_rmap_ifork	rf = {
		.accum		= { .rm_owner = ip->i_ino, },
		.rr		= rr,
		.tp		= tp,
		.whichfork	= whichfork,
	};
	struct xfs_ifork	*ifp;
	bool			done;
	int			fmt;
	int			error = 0;

	/* Do we even have data mapping extents? */
	fmt = XFS_IFORK_FORMAT(ip, whichfork);
	ifp = XFS_IFORK_PTR(ip, whichfork);
	if (!ifp)
		return 0;

	switch (fmt) {
	case XFS_DINODE_FMT_BTREE:
		error = xrep_rmap_scan_bmbt(&rf, ip, &done);
		if (error || done)
			return error;
		break;
	case XFS_DINODE_FMT_EXTENTS:
		break;
	default:
		return 0;
	}

	if (is_rt_data_fork(ip, whichfork))
		return 0;

	/* Scan incore extent cache. */
	return xrep_rmap_scan_iext(&rf, ifp);
}

/* Record reverse mappings for a file. */
STATIC int
xrep_rmap_scan_inode(
	struct xfs_mount		*mp,
	struct xfs_trans		*tp,
	xfs_ino_t			ino,
	void				*data)
{
	struct xrep_rmap		*rr = data;
	struct xfs_inode		*ip;
	unsigned int			lock_mode;
	int				error;

	/* Grab inode and lock it so we can scan it. */
	error = xfs_iget(mp, rr->sc->tp, ino, XFS_IGET_DONTCACHE, 0, &ip);
	if (error)
		return error;

	lock_mode = xfs_ilock_data_map_shared(ip);

	/* Check the data fork. */
	error = xrep_rmap_scan_ifork(rr, tp, ip, XFS_DATA_FORK);
	if (error)
		goto out_unlock;

	/* Check the attr fork. */
	error = xrep_rmap_scan_ifork(rr, tp, ip, XFS_ATTR_FORK);
	if (error)
		goto out_unlock;

	/* COW fork extents are "owned" by the refcount btree. */

out_unlock:
	xfs_iunlock(ip, lock_mode);
	xfs_irele(ip);
	return error;
}

/* Section (I): Find all AG metadata extents except for free space metadata. */

/* Add a btree block to the rmap list. */
STATIC int
xrep_rmap_visit_btblock(
	struct xfs_btree_cur	*cur,
	int			level,
	void			*priv)
{
	struct xbitmap		*bitmap = priv;
	struct xfs_buf		*bp;
	xfs_fsblock_t		fsb;

	xfs_btree_get_block(cur, level, &bp);
	if (!bp)
		return 0;

	fsb = XFS_DADDR_TO_FSB(cur->bc_mp, bp->b_bn);
	return xbitmap_set(bitmap, fsb, 1);
}

struct xrep_rmap_inodes {
	struct xrep_rmap	*rr;
	struct xbitmap		inobt_blocks;	/* INOBIT */
	struct xbitmap		ichunk_blocks;	/* ICHUNKBIT */
};

/* Record inode btree rmaps. */
STATIC int
xrep_rmap_walk_inobt(
	struct xfs_btree_cur		*cur,
	union xfs_btree_rec		*rec,
	void				*priv)
{
	struct xfs_inobt_rec_incore	irec;
	struct xrep_rmap_inodes		*ri = priv;
	struct xfs_mount		*mp = cur->bc_mp;
	xfs_fsblock_t			fsbno;
	xfs_agino_t			agino;
	xfs_agino_t			iperhole;
	unsigned int			i;
	int				error;

	/* Record the inobt blocks. */
	error = xbitmap_set_btcur_path(&ri->inobt_blocks, cur);
	if (error)
		return error;

	xfs_inobt_btrec_to_irec(mp, rec, &irec);
	agino = irec.ir_startino;

	/* Record a non-sparse inode chunk. */
	if (!xfs_inobt_issparse(irec.ir_holemask)) {
		fsbno = XFS_AGB_TO_FSB(mp, cur->bc_private.a.agno,
				XFS_AGINO_TO_AGBNO(mp, agino));

		return xbitmap_set(&ri->ichunk_blocks, fsbno,
				XFS_INODES_PER_CHUNK / mp->m_sb.sb_inopblock);
	}

	/* Iterate each chunk. */
	iperhole = max_t(xfs_agino_t, mp->m_sb.sb_inopblock,
			XFS_INODES_PER_HOLEMASK_BIT);
	for (i = 0, agino = irec.ir_startino;
	     i < XFS_INOBT_HOLEMASK_BITS;
	     i += iperhole / XFS_INODES_PER_HOLEMASK_BIT, agino += iperhole) {
		/* Skip holes. */
		if (irec.ir_holemask & (1 << i))
			continue;

		/* Record the inode chunk otherwise. */
		fsbno = XFS_AGB_TO_FSB(mp, cur->bc_private.a.agno,
				XFS_AGINO_TO_AGBNO(mp, agino));
		error = xbitmap_set(&ri->ichunk_blocks, fsbno,
				iperhole / mp->m_sb.sb_inopblock);
		if (error)
			return error;
	}

	return 0;
}

/* Collect rmaps for the blocks containing inode btrees and the inode chunks. */
STATIC int
xrep_rmap_find_inode_rmaps(
	struct xrep_rmap	*rr)
{
	struct xrep_rmap_inodes	ri = {
		.rr		= rr,
	};
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_btree_cur	*cur;
	int			error;

	xbitmap_init(&ri.inobt_blocks);
	xbitmap_init(&ri.ichunk_blocks);

	/*
	 * Iterate every record in the inobt so we can capture all the inode
	 * chunks and the blocks in the inobt itself.
	 */
	cur = xfs_inobt_init_cursor(sc->mp, sc->tp, sc->sa.agi_bp,
			sc->sa.agno, XFS_BTNUM_INO);
	error = xfs_btree_query_all(cur, xrep_rmap_walk_inobt, &ri);
	xfs_btree_del_cursor(cur, error);
	if (error)
		goto out_bitmap;

	/*
	 * Note that if there are zero records in the inobt then query_all does
	 * nothing and we have to account the empty inobt root manually.
	 */
	if (xbitmap_empty(&ri.ichunk_blocks)) {
		struct xfs_agi	*agi;
		xfs_fsblock_t	agi_root;

		agi = XFS_BUF_TO_AGI(sc->sa.agi_bp);
		agi_root = XFS_AGB_TO_FSB(sc->mp, sc->sa.agno,
				be32_to_cpu(agi->agi_root));
		error = xbitmap_set(&ri.inobt_blocks, agi_root, 1);
		if (error)
			goto out_bitmap;
	}

	/* Scan the finobt too. */
	if (xfs_sb_version_hasfinobt(&sc->mp->m_sb)) {
		cur = xfs_inobt_init_cursor(sc->mp, sc->tp, sc->sa.agi_bp,
				sc->sa.agno, XFS_BTNUM_FINO);
		error = xfs_btree_visit_blocks(cur, xrep_rmap_visit_btblock,
				&ri.inobt_blocks);
		xfs_btree_del_cursor(cur, error);
		if (error)
			goto out_bitmap;
	}

	/* Generate rmaps for everything. */
	error = xrep_rmap_stash_bitmap(rr, &ri.inobt_blocks,
			&XFS_RMAP_OINFO_INOBT);
	if (error)
		goto out_bitmap;
	error = xrep_rmap_stash_bitmap(rr, &ri.ichunk_blocks,
			&XFS_RMAP_OINFO_INODES);

out_bitmap:
	xbitmap_destroy(&ri.inobt_blocks);
	xbitmap_destroy(&ri.ichunk_blocks);
	return error;
}

/* Record a CoW staging extent. */
STATIC int
xrep_rmap_walk_cowblocks(
	struct xfs_btree_cur		*cur,
	union xfs_btree_rec		*rec,
	void				*priv)
{
	struct xbitmap			*bitmap = priv;
	struct xfs_refcount_irec	refc;
	xfs_fsblock_t			fsbno;

	xfs_refcount_btrec_to_irec(rec, &refc);
	if (refc.rc_refcount != 1)
		return -EFSCORRUPTED;

	fsbno = XFS_AGB_TO_FSB(cur->bc_mp, cur->bc_private.a.agno,
			refc.rc_startblock - XFS_REFC_COW_START);
	return xbitmap_set(bitmap, fsbno, refc.rc_blockcount);
}

/*
 * Collect rmaps for the blocks containing the refcount btree, and all CoW
 * staging extents.
 */
STATIC int
xrep_rmap_find_refcount_rmaps(
	struct xrep_rmap	*rr)
{
	struct xbitmap		refcountbt_blocks;	/* REFCBIT */
	struct xbitmap		cow_blocks;		/* COWBIT */
	union xfs_btree_irec	low;
	union xfs_btree_irec	high;
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_btree_cur	*cur;
	int			error;

	if (!xfs_sb_version_hasreflink(&sc->mp->m_sb))
		return 0;

	xbitmap_init(&refcountbt_blocks);
	xbitmap_init(&cow_blocks);

	/* refcountbt */
	cur = xfs_refcountbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.agno);
	error = xfs_btree_visit_blocks(cur, xrep_rmap_visit_btblock,
			&refcountbt_blocks);
	if (error) {
		xfs_btree_del_cursor(cur, error);
		goto out_bitmap;
	}

	/* Collect rmaps for CoW staging extents. */
	memset(&low, 0, sizeof(low));
	low.rc.rc_startblock = XFS_REFC_COW_START;
	memset(&high, 0xFF, sizeof(high));
	error = xfs_btree_query_range(cur, &low, &high,
			xrep_rmap_walk_cowblocks, &cow_blocks);
	xfs_btree_del_cursor(cur, error);
	if (error)
		goto out_bitmap;

	/* Generate rmaps for everything. */
	error = xrep_rmap_stash_bitmap(rr, &cow_blocks, &XFS_RMAP_OINFO_COW);
	if (error)
		goto out_bitmap;
	error = xrep_rmap_stash_bitmap(rr, &refcountbt_blocks,
			&XFS_RMAP_OINFO_REFC);

out_bitmap:
	xbitmap_destroy(&cow_blocks);
	xbitmap_destroy(&refcountbt_blocks);
	return error;
}

/* Generate rmaps for the AG headers (AGI/AGF/AGFL) */
STATIC int
xrep_rmap_find_agheader_rmaps(
	struct xrep_rmap	*rr)
{
	struct xfs_scrub	*sc = rr->sc;

	/* Create a record for the AG sb->agfl. */
	return xrep_rmap_stash(rr, XFS_SB_BLOCK(sc->mp),
			XFS_AGFL_BLOCK(sc->mp) - XFS_SB_BLOCK(sc->mp) + 1,
			XFS_RMAP_OWN_FS, 0, 0);
}

/* Generate rmaps for the log, if it's in this AG. */
STATIC int
xrep_rmap_find_log_rmaps(
	struct xrep_rmap	*rr)
{
	struct xfs_scrub	*sc = rr->sc;

	if (sc->mp->m_sb.sb_logstart == 0 ||
	    XFS_FSB_TO_AGNO(sc->mp, sc->mp->m_sb.sb_logstart) != sc->sa.agno)
		return 0;

	return xrep_rmap_stash(rr,
			XFS_FSB_TO_AGBNO(sc->mp, sc->mp->m_sb.sb_logstart),
			sc->mp->m_sb.sb_logblocks, XFS_RMAP_OWN_LOG, 0, 0);
}

/*
 * Generate all the reverse-mappings for this AG, a list of the old rmapbt
 * blocks, and the new btreeblks count.  Figure out if we have enough free
 * space to reconstruct the inode btrees.  The caller must clean up the lists
 * if anything goes wrong.  This implements section (I) above.
 */
STATIC int
xrep_rmap_find_rmaps(
	struct xrep_rmap	*rr)
{
	struct xfs_scrub	*sc = rr->sc;
	int			error;

	/* Iterate all AGs for inodes rmaps. */
	error = xfs_iwalk(sc->mp, sc->tp, 0, 0, xrep_rmap_scan_inode, 0, rr);
	if (error)
		return error;

	/* Find all the other per-AG metadata. */
	error = xrep_rmap_find_inode_rmaps(rr);
	if (error)
		return error;

	error = xrep_rmap_find_refcount_rmaps(rr);
	if (error)
		return error;

	error = xrep_rmap_find_agheader_rmaps(rr);
	if (error)
		return error;

	return xrep_rmap_find_log_rmaps(rr);
}

/* Section (II): Reserving space for new rmapbt and setting free space bitmap */

struct xrep_rmap_agfl {
	struct xbitmap		*bitmap;
	xfs_agnumber_t		agno;
};

/* Add an AGFL block to the rmap list. */
STATIC int
xrep_rmap_walk_agfl(
	struct xfs_mount	*mp,
	xfs_agblock_t		bno,
	void			*priv)
{
	struct xrep_rmap_agfl	*ra = priv;

	return xbitmap_set(ra->bitmap, XFS_AGB_TO_FSB(mp, ra->agno, bno), 1);
}

/*
 * Run one round of reserving space for the new rmapbt and recomputing the
 * number of blocks needed to store the previously observed rmapbt records and
 * the ones we'll create for the free space metadata.  When we don't need more
 * blocks, return a bitmap of OWN_AG extents in @freesp_blocks and set @done to
 * true.
 */
STATIC int
xrep_rmap_try_reserve(
	struct xrep_rmap	*rr,
	struct xfs_btree_bload	*rmap_bload,
	uint64_t		nr_records,
	struct xbitmap		*freesp_blocks,
	uint64_t		*blocks_reserved,
	bool			*done)
{
	struct xrep_rmap_agfl	ra = {
		.bitmap		= freesp_blocks,
		.agno		= rr->sc->sa.agno,
	};
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_btree_cur	*cur;
	struct xrep_newbt_resv	*resv, *n;
	uint64_t		nr_blocks;	/* RMB */
	uint64_t		freesp_records;
	int			error;

	nr_blocks = rmap_bload->nr_blocks;

	/* Reserve the space we'll need for the new btree. */
	error = xrep_newbt_reserve_space(&rr->new_btree_info,
			nr_blocks - *blocks_reserved);
	if (error)
		return error;

	*blocks_reserved = rmap_bload->nr_blocks;

	/* Clear everything in the bitmap. */
	xbitmap_destroy(freesp_blocks);

	/* Set all the bnobt blocks in the bitmap. */
	cur = xfs_allocbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.agno, XFS_BTNUM_BNO);
	error = xfs_btree_visit_blocks(cur, xrep_rmap_visit_btblock,
			freesp_blocks);
	xfs_btree_del_cursor(cur, error);
	if (error)
		return error;

	/* Set all the cntbt blocks in the bitmap. */
	cur = xfs_allocbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.agno, XFS_BTNUM_CNT);
	error = xfs_btree_visit_blocks(cur, xrep_rmap_visit_btblock,
			freesp_blocks);
	xfs_btree_del_cursor(cur, error);
	if (error)
		return error;

	/* Record our new btreeblks value. */
	rr->freesp_btblocks = xbitmap_hweight(freesp_blocks) - 2;

	/* Set all the new rmapbt blocks in the bitmap. */
	for_each_xrep_newbt_reservation(&rr->new_btree_info, resv, n) {
		error = xbitmap_set(freesp_blocks, resv->fsbno, resv->len);
		if (error)
			return error;
	}

	/* Set all the AGFL blocks in the bitmap. */
	error = xfs_agfl_walk(sc->mp, XFS_BUF_TO_AGF(sc->sa.agf_bp),
			sc->sa.agfl_bp, xrep_rmap_walk_agfl, &ra);
	if (error)
		return error;

	/* Count the extents in the bitmap. */
	freesp_records = xbitmap_count_set_regions(freesp_blocks);

	/* Compute how many blocks we'll need for all the rmaps. */
	cur = xfs_rmapbt_stage_cursor(sc->mp, sc->tp,
			&rr->new_btree_info.afake, sc->sa.agno);
	error = xfs_btree_bload_compute_geometry(cur, rmap_bload,
			nr_records + freesp_records);
	xfs_btree_del_cursor(cur, error);

	/* We're done when we don't need more blocks. */
	*done = nr_blocks >= rmap_bload->nr_blocks;
	return 0;
}

/*
 * Iteratively reserve space for rmap btree while recording OWN_AG rmaps for
 * the free space metadata.  This implements section (II) above.
 */
STATIC int
xrep_rmap_reserve_space(
	struct xrep_rmap	*rr,
	struct xfs_btree_bload	*rmap_bload)
{
	struct xbitmap		freesp_blocks;	/* AGBIT */
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_btree_cur	*rmap_cur;
	uint64_t		nr_records;	/* NR */
	uint64_t		blocks_reserved = 0;
	bool			done = false;
	int			error;

	nr_records = xfbma_length(rr->rmap_records);

	/*
	 * Prepare to construct the new btree by reserving disk space for the
	 * new btree and setting up all the accounting information we'll need
	 * to root the new btree while it's under construction and before we
	 * attach it to the AG header.
	 */
	xrep_newbt_init_ag(&rr->new_btree_info, sc, &XFS_RMAP_OINFO_SKIP_UPDATE,
			XFS_AGB_TO_FSB(sc->mp, sc->sa.agno,
				       XFS_RMAP_BLOCK(sc->mp)),
			XFS_AG_RESV_RMAPBT);

	/* Compute how many blocks we'll need for the rmaps collected so far. */
	rmap_cur = xfs_rmapbt_stage_cursor(sc->mp, sc->tp,
			&rr->new_btree_info.afake, sc->sa.agno);
	error = xfs_btree_bload_compute_geometry(rmap_cur, rmap_bload,
			nr_records);
	xfs_btree_del_cursor(rmap_cur, error);
	if (error)
		return error;

	xbitmap_init(&freesp_blocks);

	/*
	 * Iteratively reserve space for the new rmapbt and recompute the
	 * number of blocks needed to store the previously observed rmapbt
	 * records and the ones we'll create for the free space metadata.
	 * Finish when we don't need more blocks.
	 */
	do {
		error = xrep_rmap_try_reserve(rr, rmap_bload, nr_records,
				&freesp_blocks, &blocks_reserved, &done);
		if (error)
			goto out_bitmap;
	} while (!done);

	/* Emit rmaps for everything in the free space bitmap. */
	error = xrep_rmap_stash_bitmap(rr, &freesp_blocks, &XFS_RMAP_OINFO_AG);

out_bitmap:
	xbitmap_destroy(&freesp_blocks);
	return error;
}

/* Section (III): Building the new rmap btree. */

/* Update the AGF counters. */
STATIC int
xrep_rmap_reset_counters(
	struct xrep_rmap	*rr)
{
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_perag	*pag = sc->sa.pag;
	struct xfs_agf		*agf;
	struct xfs_buf		*bp;
	xfs_agblock_t		rmap_btblocks;

	agf = XFS_BUF_TO_AGF(sc->sa.agf_bp);

	/*
	 * Mark the pagf information stale and use the accessor function to
	 * forcibly reload it from the values we just logged.  We still own the
	 * AGF buffer so we can safely ignore bp.
	 */
	ASSERT(pag->pagf_init);
	pag->pagf_init = 0;

	rmap_btblocks = rr->new_btree_info.afake.af_blocks - 1;
	agf->agf_btreeblks = cpu_to_be32(rr->freesp_btblocks + rmap_btblocks);
	xfs_alloc_log_agf(sc->tp, sc->sa.agf_bp, XFS_AGF_BTREEBLKS);

	return xfs_alloc_read_agf(sc->mp, sc->tp, sc->sa.agno, 0, &bp);
}

/* Retrieve rmapbt data for bulk load. */
STATIC int
xrep_rmap_get_data(
	struct xfs_btree_cur	*cur,
	void			*priv)
{
	struct xfs_rmap_irec	*irec = &cur->bc_rec.r;
	struct xrep_rmap_extent	rec;
	struct xrep_rmap	*rr = priv;
	int			error;

	do {
		error = xfbma_get(rr->rmap_records, rr->iter++, &rec);
	} while (error == 0 && xfbma_is_null(rr->rmap_records, &rec));

	if (error)
		return error;

	irec->rm_startblock = rec.startblock;
	irec->rm_blockcount = rec.blockcount;
	irec->rm_owner = rec.owner;
	return xfs_rmap_irec_offset_unpack(rec.offset, irec);
}

/* Feed one of the new btree blocks to the bulk loader. */
STATIC int
xrep_rmap_bload_alloc(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	void			*priv)
{
	struct xrep_rmap        *rr = priv;

	return xrep_newbt_alloc_block(cur, &rr->new_btree_info, ptr);
}

/*
 * Use the collected rmap information to stage a new rmap btree.  If this is
 * successful we'll return with the new btree root information logged to the
 * repair transaction but not yet committed.  This implements section (III)
 * above.
 */
STATIC int
xrep_rmap_build_new_tree(
	struct xrep_rmap	*rr)
{
	struct xfs_btree_bload	rmap_bload = {
		.get_data	= xrep_rmap_get_data,
		.alloc_block	= xrep_rmap_bload_alloc,
	};
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_btree_cur	*rmap_cur;
	int			error;

	xrep_bload_estimate_slack(sc, &rmap_bload);

	/*
	 * Initialize @rr->new_btree_info, reserve space for the new rmapbt,
	 * and compute OWN_AG rmaps.
	 */
	error = xrep_rmap_reserve_space(rr, &rmap_bload);
	if (error)
		return error;

	/*
	 * Sort the rmap records by startblock or else the btree records
	 * will be in the wrong order.
	 */
	error = xfbma_sort(rr->rmap_records, xrep_rmap_extent_cmp);
	if (error)
		goto err_newbt;

	/* Add all observed rmap records. */
	rr->iter = 0;
	rmap_cur = xfs_rmapbt_stage_cursor(sc->mp, sc->tp,
			&rr->new_btree_info.afake, sc->sa.agno);
	error = xfs_btree_bload(rmap_cur, &rmap_bload, rr);
	if (error)
		goto err_cur;

	/*
	 * Install the new btree in the AG header.  After this point the old
	 * btree is no longer accessible and the new tree is live.
	 *
	 * Note: We re-read the AGF here to ensure the buffer type is set
	 * properly.  Since we built a new tree without attaching to the AGF
	 * buffer, the buffer item may have fallen off the buffer.  This ought
	 * to succeed since the AGF is held across transaction rolls.
	 */
	error = xfs_read_agf(sc->mp, sc->tp, sc->sa.agno, 0, &sc->sa.agf_bp);
	if (error)
		goto err_cur;

	/* Commit our new btree. */
	xfs_rmapbt_commit_staged_btree(rmap_cur, sc->sa.agf_bp);
	xfs_btree_del_cursor(rmap_cur, 0);

	/* Reset the AGF counters now that we've changed the btree shape. */
	error = xrep_rmap_reset_counters(rr);
	if (error)
		goto err_newbt;

	/* Dispose of any unused blocks and the accounting information. */
	xrep_newbt_destroy(&rr->new_btree_info, error);

	return xrep_roll_ag_trans(sc);
err_cur:
	xfs_btree_del_cursor(rmap_cur, error);
err_newbt:
	xrep_newbt_destroy(&rr->new_btree_info, error);
	return error;
}

/* Section (IV): Reaping the old btree. */

/* Subtract each free extent in the bnobt from the rmap gaps. */
STATIC int
xrep_rmap_find_freesp(
	struct xfs_btree_cur		*cur,
	struct xfs_alloc_rec_incore	*rec,
	void				*priv)
{
	struct xbitmap			*bitmap = priv;
	xfs_fsblock_t			fsb;

	fsb = XFS_AGB_TO_FSB(cur->bc_mp, cur->bc_private.a.agno,
			rec->ar_startblock);
	xbitmap_clear(bitmap, fsb, rec->ar_blockcount);
	return 0;
}

/*
 * Reap the old rmapbt blocks.  Now that the rmapbt is fully rebuilt, we make
 * a list of gaps in the rmap records and a list of the extents mentioned in
 * the bnobt.  Any block that's in the new rmapbt gap list but not mentioned
 * in the bnobt is a block from the old rmapbt and can be removed.
 */
STATIC int
xrep_rmap_remove_old_tree(
	struct xrep_rmap	*rr)
{
	struct xbitmap		rmap_gaps;
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_agf		*agf;
	struct xfs_btree_cur	*cur;
	xfs_fsblock_t		next_fsb = XFS_AGB_TO_FSB(mp, sc->sa.agno, 0);
	xfs_fsblock_t		agend_fsb;
	uint64_t		nr_records = xfbma_length(rr->rmap_records);
	int			error;

	xbitmap_init(&rmap_gaps);

	/* Compute free space from the new rmapbt. */
	for (rr->iter = 0; rr->iter < nr_records; rr->iter++) {
		struct xrep_rmap_extent	rec;
		xfs_fsblock_t	fsbno;

		error = xfbma_get(rr->rmap_records, rr->iter, &rec);
		if (error)
			goto out_bitmap;

		/* Record the free space we find. */
		fsbno = XFS_AGB_TO_FSB(mp, sc->sa.agno, rec.startblock);
		if (fsbno > next_fsb) {
			error = xbitmap_set(&rmap_gaps, next_fsb,
					fsbno - next_fsb);
			if (error)
				goto out_bitmap;
		}
		next_fsb = max_t(xfs_fsblock_t, next_fsb,
				fsbno + rec.blockcount);
	}

	/* Insert a record for space between the last rmap and EOAG. */
	agf = XFS_BUF_TO_AGF(sc->sa.agf_bp);
	agend_fsb = XFS_AGB_TO_FSB(mp, sc->sa.agno,
			be32_to_cpu(agf->agf_length));
	if (next_fsb < agend_fsb) {
		error = xbitmap_set(&rmap_gaps, next_fsb,
				agend_fsb - next_fsb);
		if (error)
			goto out_bitmap;
	}

	/* Compute free space from the existing bnobt. */
	cur = xfs_allocbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.agno, XFS_BTNUM_BNO);
	error = xfs_alloc_query_all(cur, xrep_rmap_find_freesp, &rmap_gaps);
	xfs_btree_del_cursor(cur, error);
	if (error)
		goto out_bitmap;

	/*
	 * Free the "free" blocks that the new rmapbt knows about but
	 * the bnobt doesn't.  These are the old rmapbt blocks.
	 */
	error = xrep_reap_extents(sc, &rmap_gaps, &XFS_RMAP_OINFO_ANY_OWNER,
			XFS_AG_RESV_RMAPBT);
	if (error)
		goto out_bitmap;

	sc->flags |= XREP_RESET_PERAG_RESV;
out_bitmap:
	xbitmap_destroy(&rmap_gaps);
	return error;
}

/* Repair the rmap btree for some AG. */
int
xrep_rmapbt(
	struct xfs_scrub	*sc)
{
	struct xrep_rmap	*rr;
	int			error;

	rr = kmem_zalloc(sizeof(struct xrep_rmap), KM_NOFS | KM_MAYFAIL);
	if (!rr)
		return -ENOMEM;
	rr->sc = sc;

	xchk_perag_get(sc->mp, &sc->sa);

	/* Set up some storage */
	rr->rmap_records = xfbma_init(sizeof(struct xrep_rmap_extent));
	if (IS_ERR(rr->rmap_records)) {
		error = PTR_ERR(rr->rmap_records);
		goto out_rr;
	}

	/*
	 * Collect rmaps for everything in this AG that isn't space metadata.
	 * These rmaps won't change even as we try to allocate blocks.
	 */
	error = xrep_rmap_find_rmaps(rr);
	if (error)
		goto out_records;

	/* Rebuild the rmap information. */
	error = xrep_rmap_build_new_tree(rr);
	if (error)
		goto out_records;

	/* Kill the old tree. */
	error = xrep_rmap_remove_old_tree(rr);

out_records:
	xfbma_destroy(rr->rmap_records);
out_rr:
	kmem_free(rr);
	return error;
}
