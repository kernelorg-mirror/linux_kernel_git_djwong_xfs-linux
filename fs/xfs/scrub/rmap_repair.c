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
#include "xfs_ag.h"
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
xrep_setup_ag_rmapbt(
	struct xfs_scrub	*sc)
{
	/* For now this is a placeholder until we land other pieces. */
	return 0;
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
	struct xfs_btree_bload	rmap_bload;

	/* rmap records generated from primary metadata */
	struct xfarray		*rmap_records;

	struct xfs_scrub	*sc;

	/* staged rmap btree cursor */
	struct xfs_btree_cur	*cur;

	/* get_record()'s position in the free space record array. */
	uint64_t		iter;

	/* inode scan cursor */
	struct xchk_iscan	iscan;

	/* bnobt/cntbt contribution to btreeblks */
	xfs_agblock_t		freesp_btblocks;

	/* old agf_rmap_blocks counter */
	unsigned int		old_rmapbt_fsbcount;
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
	if (error)
		ASSERT(error == 0);

	error = xfs_rmap_irec_offset_unpack(bp->offset, &br);
	if (error)
		ASSERT(error == 0);

	return xfs_rmap_compare(&ar, &br);
}

/* Make sure there's nothing funny about this mapping. */
STATIC int
xrep_rmap_check_mapping(
	struct xfs_scrub	*sc,
	const struct xfs_rmap_irec *rec)
{
	bool			is_freesp;
	int			error;

	if (rec->rm_owner == XFS_RMAP_OWN_FS) {
		/* Static metadata only exists at the start of the AG. */
		if (rec->rm_startblock != 0)
			return -EFSCORRUPTED;
	} else {
		/* Check that this is within an AG and not static metadata. */
		if (!xfs_verify_agbext(sc->mp, sc->sa.pag->pag_agno,
					rec->rm_startblock,
					rec->rm_blockcount))
			return -EFSCORRUPTED;
	}

	/* Check for a valid fork offset, if applicable. */
	if (!XFS_RMAP_NON_INODE_OWNER(rec->rm_owner) &&
	    !(rec->rm_flags & XFS_RMAP_BMBT_BLOCK) &&
	    !xfs_verify_fileext(sc->mp, rec->rm_offset, rec->rm_blockcount))
		return -EFSCORRUPTED;

	/* Make sure this isn't free space. */
	error = xfs_alloc_has_record(sc->sa.bno_cur, rec->rm_startblock,
			rec->rm_blockcount, &is_freesp);
	if (error)
		return error;
	if (is_freesp)
		return -EFSCORRUPTED;

	return 0;
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

	trace_xrep_rmap_found(sc->mp, sc->sa.pag->pag_agno, &rmap);

	rre.offset = xfs_rmap_irec_offset_pack(&rmap);
	return xfarray_append(rr->rmap_records, &rre);
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

	return xbitmap_walk(bitmap, xrep_rmap_stash_run, &rsr);
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

	/* Which inode fork? */
	int			whichfork;
};

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

	if (XFS_FSB_TO_AGNO(mp, rec->br_startblock) !=
			rf->rr->sc->sa.pag->pag_agno)
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

/* Add a btree block to the bitmap. */
STATIC int
xrep_rmap_visit_iroot_btree_block(
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

	fsb = XFS_DADDR_TO_FSB(cur->bc_mp, xfs_buf_daddr(bp));
	if (XFS_FSB_TO_AGNO(cur->bc_mp, fsb) != rf->rr->sc->sa.pag->pag_agno)
		return 0;

	return xbitmap_set(&rf->bmbt_blocks, fsb, 1);
}

/*
 * Iterate a metadata btree rooted in an inode to collect rmap records for
 * anything in this fork that matches the AG.
 */
STATIC int
xrep_rmap_scan_iroot_btree(
	struct xrep_rmap_ifork	*rf,
	struct xfs_btree_cur	*cur)
{
	struct xfs_owner_info	oinfo;
	struct xrep_rmap	*rr = rf->rr;
	int			error;

	xbitmap_init(&rf->bmbt_blocks);

	/* Record all the blocks in the btree itself. */
	error = xfs_btree_visit_blocks(cur, xrep_rmap_visit_iroot_btree_block,
			XFS_BTREE_VISIT_ALL, rf);
	if (error)
		goto out;

	/* Emit rmaps for the btree blocks. */
	xfs_rmap_ino_bmbt_owner(&oinfo, rf->accum.rm_owner, rf->whichfork);
	error = xrep_rmap_stash_bitmap(rr, &rf->bmbt_blocks, &oinfo);
	if (error)
		goto out;

	/* Stash any remaining accumulated rmaps. */
	error = xrep_rmap_stash_accumulated(rf);
out:
	xbitmap_destroy(&rf->bmbt_blocks);
	return error;
}

static inline bool
is_rt_data_fork(
	struct xfs_inode	*ip,
	int			whichfork)
{
	return XFS_IS_REALTIME_INODE(ip) && whichfork == XFS_DATA_FORK;
}

/*
 * Iterate the block mapping btree to collect rmap records for anything in this
 * fork that matches the AG.  Sets @mappings_done to true if we've scanned the
 * block mappings in this fork.
 */
STATIC int
xrep_rmap_scan_bmbt(
	struct xrep_rmap_ifork	*rf,
	struct xfs_inode	*ip,
	bool			*mappings_done)
{
	struct xrep_rmap	*rr = rf->rr;
	struct xfs_btree_cur	*cur;
	struct xfs_ifork	*ifp;
	int			error;

	*mappings_done = false;
	ifp = XFS_IFORK_PTR(ip, rf->whichfork);
	cur = xfs_bmbt_init_cursor(rr->sc->mp, rr->sc->tp, ip, rf->whichfork);

	if (!xfs_ifork_is_realtime(ip, rf->whichfork) &&
	    xfs_need_iread_extents(ifp)) {
		/*
		 * If the incore extent cache isn't loaded, scan the bmbt for
		 * mapping records.  This avoids loading the incore extent
		 * tree, which will increase memory pressure at a time when
		 * we're trying to run as quickly as we possibly can.  Ignore
		 * realtime extents.
		 */
		error = xfs_bmap_query_all(cur, xrep_rmap_visit_bmbt, rf);
		if (error)
			goto out_cur;

		*mappings_done = true;
	}

	/* Scan for the bmbt blocks, which always live on the data device. */
	error = xrep_rmap_scan_iroot_btree(rf, cur);
out_cur:
	xfs_btree_del_cursor(cur, error);
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
	struct xfs_inode	*ip,
	int			whichfork)
{
	struct xrep_rmap_ifork	rf = {
		.accum		= { .rm_owner = ip->i_ino, },
		.rr		= rr,
		.whichfork	= whichfork,
	};
	struct xfs_ifork	*ifp = XFS_IFORK_PTR(ip, whichfork);
	int			error = 0;

	if (!ifp)
		return 0;

	if (ifp->if_format == XFS_DINODE_FMT_BTREE) {
		bool		mappings_done;

		/*
		 * Scan the bmap btree for data device mappings.  This includes
		 * the btree blocks themselves, even if this is a realtime
		 * file.
		 */
		error = xrep_rmap_scan_bmbt(&rf, ip, &mappings_done);
		if (error || mappings_done)
			return error;
	} else if (ifp->if_format != XFS_DINODE_FMT_EXTENTS) {
		return 0;
	}

	/* Scan incore extent cache if this isn't a realtime file. */
	if (xfs_ifork_is_realtime(ip, whichfork))
		return 0;

	return xrep_rmap_scan_iext(&rf, ifp);
}

/* Record reverse mappings for a file. */
STATIC int
xrep_rmap_scan_inode(
	struct xrep_rmap	*rr,
	struct xfs_inode	*ip)
{
	unsigned int		lock_mode;
	int			error;

	xfs_ilock(ip, XFS_IOLOCK_SHARED | XFS_MMAPLOCK_SHARED);
	lock_mode = xfs_ilock_data_map_shared(ip);

	/* Check the data fork. */
	error = xrep_rmap_scan_ifork(rr, ip, XFS_DATA_FORK);
	if (error)
		goto out_unlock;

	/* Check the attr fork. */
	error = xrep_rmap_scan_ifork(rr, ip, XFS_ATTR_FORK);
	if (error)
		goto out_unlock;

	/* COW fork extents are "owned" by the refcount btree. */

	xchk_iscan_mark_visited(&rr->iscan, ip);
out_unlock:
	xfs_iunlock(ip, XFS_IOLOCK_SHARED | XFS_MMAPLOCK_SHARED | lock_mode);
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

	fsb = XFS_DADDR_TO_FSB(cur->bc_mp, xfs_buf_daddr(bp));
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
	const union xfs_btree_rec	*rec,
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
		fsbno = XFS_AGB_TO_FSB(mp, cur->bc_ag.pag->pag_agno,
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
		fsbno = XFS_AGB_TO_FSB(mp, cur->bc_ag.pag->pag_agno,
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
	int			error;

	xbitmap_init(&ri.inobt_blocks);
	xbitmap_init(&ri.ichunk_blocks);

	/*
	 * Iterate every record in the inobt so we can capture all the inode
	 * chunks and the blocks in the inobt itself.
	 */
	error = xfs_btree_query_all(sc->sa.ino_cur, xrep_rmap_walk_inobt, &ri);
	if (error)
		goto out_bitmap;

	/*
	 * Note that if there are zero records in the inobt then query_all does
	 * nothing and we have to account the empty inobt root manually.
	 */
	if (xbitmap_empty(&ri.ichunk_blocks)) {
		struct xfs_agi	*agi = sc->sa.agi_bp->b_addr;
		xfs_fsblock_t	agi_root;

		agi_root = XFS_AGB_TO_FSB(sc->mp, sc->sa.pag->pag_agno,
				be32_to_cpu(agi->agi_root));
		error = xbitmap_set(&ri.inobt_blocks, agi_root, 1);
		if (error)
			goto out_bitmap;
	}

	/* Scan the finobt too. */
	if (xfs_has_finobt(sc->mp)) {
		error = xfs_btree_visit_blocks(sc->sa.fino_cur,
				xrep_rmap_visit_btblock, XFS_BTREE_VISIT_ALL,
				&ri.inobt_blocks);
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
	const union xfs_btree_rec	*rec,
	void				*priv)
{
	struct xbitmap			*bitmap = priv;
	struct xfs_refcount_irec	refc;
	xfs_fsblock_t			fsbno;

	xfs_refcount_btrec_to_irec(rec, &refc);
	if (refc.rc_refcount != 1)
		return -EFSCORRUPTED;

	fsbno = XFS_AGB_TO_FSB(cur->bc_mp, cur->bc_ag.pag->pag_agno,
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
	int			error;

	if (!xfs_has_reflink(sc->mp))
		return 0;

	xbitmap_init(&refcountbt_blocks);
	xbitmap_init(&cow_blocks);

	/* refcountbt */
	error = xfs_btree_visit_blocks(sc->sa.refc_cur, xrep_rmap_visit_btblock,
			XFS_BTREE_VISIT_ALL, &refcountbt_blocks);
	if (error)
		goto out_bitmap;

	/* Collect rmaps for CoW staging extents. */
	memset(&low, 0, sizeof(low));
	low.rc.rc_startblock = XFS_REFC_COW_START;
	memset(&high, 0xFF, sizeof(high));
	error = xfs_btree_query_range(sc->sa.refc_cur, &low, &high,
			xrep_rmap_walk_cowblocks, &cow_blocks);
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
	    XFS_FSB_TO_AGNO(sc->mp, sc->mp->m_sb.sb_logstart) !=
				sc->sa.pag->pag_agno)
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
	struct xchk_iscan	*iscan = &rr->iscan;
	struct xchk_ag		*sa = &sc->sa;
	int			error;

	/* Find all the per-AG metadata. */
	xrep_ag_btcur_init(sc, &sc->sa);

	error = xrep_rmap_find_inode_rmaps(rr);
	if (error)
		goto end_agscan;

	error = xrep_rmap_find_refcount_rmaps(rr);
	if (error)
		goto end_agscan;

	error = xrep_rmap_find_agheader_rmaps(rr);
	if (error)
		goto end_agscan;

	error = xrep_rmap_find_log_rmaps(rr);
end_agscan:
	xchk_ag_btcur_free(&sc->sa);
	if (error)
		return error;

	/*
	 * Set up for a potentially lengthy filesystem scan by reducing our
	 * transaction resource usage for the duration.  Specifically:
	 *
	 * Unlock the AG header buffers and cancel the transaction to release
	 * the log grant space while we scan the filesystem.
	 *
	 * Create a new empty transaction to eliminate the possibility of the
	 * inode scan deadlocking on cyclical metadata.
	 *
	 * We pass the empty transaction to the file scanning function to avoid
	 * repeatedly cycling empty transactions.  This can be done even though
	 * we take the IOLOCK to quiesce the file because empty transactions
	 * do not take sb_internal.
	 */
	sa->agfl_bp = NULL;
	sa->agf_bp = NULL;
	sa->agi_bp = NULL;
	xchk_trans_cancel(sc);
	error = xchk_trans_alloc_empty(sc);
	if (error)
		return error;

	/* Iterate all AGs for inodes rmaps. */
	while ((error = xchk_iscan_advance(sc, iscan)) == 1) {
		struct xfs_inode	*ip;

		error = xchk_iscan_iget(sc, iscan, &ip);
		if (error == -EAGAIN)
			continue;
		if (error)
			break;

		error = xrep_rmap_scan_inode(rr, ip);
		xchk_irele(sc, ip);
		if (error)
			break;

		if (xchk_should_terminate(sc, &error))
			break;
	}
	if (error)
		return error;

	/*
	 * Switch out for a real transaction and lock the AG headers in
	 * preparation for building a new tree.
	 */
	xchk_trans_cancel(sc);
	error = xchk_setup_fs(sc);
	if (error)
		return error;
	return xchk_perag_lock(sc);
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
	uint64_t		nr_records,
	struct xbitmap		*freesp_blocks,
	uint64_t		*blocks_reserved,
	bool			*done)
{
	struct xrep_rmap_agfl	ra = {
		.bitmap		= freesp_blocks,
		.agno		= rr->sc->sa.pag->pag_agno,
	};
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_btree_cur	*cur;
	struct xrep_newbt_resv	*resv, *n;
	struct xfs_agf		*agf = sc->sa.agf_bp->b_addr;
	uint64_t		nr_blocks;	/* RMB */
	uint64_t		freesp_records;
	int			error;

	/*
	 * We're going to recompute rmap_bload.nr_blocks at the end of this
	 * function to reflect however many btree blocks we need to store all
	 * the rmap records (including the ones that reflect the changes we
	 * made to support the new rmapbt blocks), so we save the old value
	 * here so we can decide if we've reserved enough blocks.
	 */
	nr_blocks = rr->rmap_bload.nr_blocks;

	/*
	 * Make sure we've reserved enough space for the new btree.  This can
	 * change the shape of the free space btrees, which can cause secondary
	 * interactions with the rmap records because all three space btrees
	 * have the same rmap owner.  We'll account for all that below.
	 */
	error = xrep_newbt_alloc_blocks(&rr->new_btree_info,
			nr_blocks - *blocks_reserved);
	if (error)
		return error;

	*blocks_reserved = rr->rmap_bload.nr_blocks;

	/* Clear everything in the bitmap. */
	xbitmap_destroy(freesp_blocks);

	/* Set all the bnobt blocks in the bitmap. */
	cur = xfs_allocbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.pag, XFS_BTNUM_BNO);
	error = xfs_btree_visit_blocks(cur, xrep_rmap_visit_btblock,
			XFS_BTREE_VISIT_ALL, freesp_blocks);
	xfs_btree_del_cursor(cur, error);
	if (error)
		return error;

	/* Set all the cntbt blocks in the bitmap. */
	cur = xfs_allocbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.pag, XFS_BTNUM_CNT);
	error = xfs_btree_visit_blocks(cur, xrep_rmap_visit_btblock,
			XFS_BTREE_VISIT_ALL, freesp_blocks);
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
	error = xfs_agfl_walk(sc->mp, agf, sc->sa.agfl_bp, xrep_rmap_walk_agfl,
			&ra);
	if (error)
		return error;

	/* Count the extents in the bitmap. */
	freesp_records = xbitmap_count_set_regions(freesp_blocks);

	/* Compute how many blocks we'll need for all the rmaps. */
	error = xfs_btree_bload_compute_geometry(rr->cur, &rr->rmap_bload,
			nr_records + freesp_records);
	if (error)
		return error;

	/* We're done when we don't need more blocks. */
	*done = nr_blocks >= rr->rmap_bload.nr_blocks;
	return 0;
}

/*
 * Iteratively reserve space for rmap btree while recording OWN_AG rmaps for
 * the free space metadata.  This implements section (II) above.
 */
STATIC int
xrep_rmap_reserve_space(
	struct xrep_rmap	*rr)
{
	struct xbitmap		freesp_blocks;	/* AGBIT */
	uint64_t		nr_records;	/* NR */
	uint64_t		blocks_reserved = 0;
	bool			done = false;
	int			error;

	nr_records = xfarray_length(rr->rmap_records);

	/* Compute how many blocks we'll need for the rmaps collected so far. */
	error = xfs_btree_bload_compute_geometry(rr->cur, &rr->rmap_bload,
			nr_records);
	if (error)
		return error;

	/* Last chance to abort before we start committing fixes. */
	if (xchk_should_terminate(rr->sc, &error))
		return error;

	xbitmap_init(&freesp_blocks);

	/*
	 * Iteratively reserve space for the new rmapbt and recompute the
	 * number of blocks needed to store the previously observed rmapbt
	 * records and the ones we'll create for the free space metadata.
	 * Finish when we don't need more blocks.
	 */
	do {
		error = xrep_rmap_try_reserve(rr, nr_records, &freesp_blocks,
				&blocks_reserved, &done);
		if (error)
			goto out_bitmap;
	} while (!done);

	/* Emit rmaps for everything in the free space bitmap. */
	xrep_ag_btcur_init(rr->sc, &rr->sc->sa);
	error = xrep_rmap_stash_bitmap(rr, &freesp_blocks, &XFS_RMAP_OINFO_AG);
	xchk_ag_btcur_free(&rr->sc->sa);

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
	struct xfs_agf		*agf = sc->sa.agf_bp->b_addr;
	struct xfs_buf		*bp;
	xfs_agblock_t		rmap_btblocks;

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

	return xfs_alloc_read_agf(sc->mp, sc->tp, sc->sa.pag->pag_agno, 0, &bp);
}

/* Retrieve rmapbt data for bulk load. */
STATIC int
xrep_rmap_get_record(
	struct xfs_btree_cur	*cur,
	void			*priv)
{
	struct xrep_rmap_extent	rec;
	struct xfs_rmap_irec	*irec = &cur->bc_rec.r;
	struct xrep_rmap	*rr = priv;
	int			error;

	error = xfarray_load_next(rr->rmap_records, &rr->iter, &rec);
	if (error)
		return error;

	irec->rm_startblock = rec.startblock;
	irec->rm_blockcount = rec.blockcount;
	irec->rm_owner = rec.owner;
	error = xfs_rmap_irec_offset_unpack(rec.offset, irec);
	if (error)
		return error;

	return xrep_rmap_check_mapping(rr->sc, irec);
}

/* Feed one of the new btree blocks to the bulk loader. */
STATIC int
xrep_rmap_claim_block(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	void			*priv)
{
	struct xrep_rmap        *rr = priv;
	int			error;

	error = xrep_newbt_relog_efis(&rr->new_btree_info);
	if (error)
		return error;

	return xrep_newbt_claim_block(cur, &rr->new_btree_info, ptr);
}

/* Custom allocation function for new rmap btrees. */
STATIC int
xrep_rmap_alloc_vextent(
	struct xfs_scrub	*sc,
	struct xfs_alloc_arg	*args)
{
	int			error;

	/*
	 * We don't want an rmap update on the allocation, since we iteratively
	 * compute the OWN_AG records /after/ allocating blocks for the records
	 * that we already know we need to store.  Therefore, fix the freelist
	 * with the NORMAP flag set so that we don't also try to create an rmap
	 * for new AGFL blocks.
	 */
	error = xrep_fix_freelist(sc, XFS_ALLOC_FLAG_NORMAP);
	if (error)
		return error;

	/*
	 * If the transaction is dirty, we fixed the freelist either by moving
	 * blocks from the free space btrees, or by removing blocks from the
	 * AGFL and queueing an EFI to free the block.  Later on, we will need
	 * to compare gaps in the new recordset against the block usage of all
	 * OWN_AG owners in order to free the old btree's blocks, which means
	 * that we can't have EFIs for former AGFL blocks attached to the
	 * repair transaction when we commit the new btree.
	 *
	 * Therefore, log and hold all three AG headers so that we don't lose
	 * our hold on the resources needed to commit the new btree, and call
	 * defer_finish to commit anything that fix_freelist may have added to
	 * the transaction.
	 */
	if (sc->tp->t_flags & XFS_TRANS_DIRTY) {
		struct xfs_trans	*tp = sc->tp;
		struct xfs_buf		*agfl_bp = sc->sa.agfl_bp;

		xfs_ialloc_log_agi(tp, sc->sa.agi_bp, XFS_AGI_MAGICNUM);
		xfs_alloc_log_agf(tp, sc->sa.agf_bp, XFS_AGF_MAGICNUM);
		xfs_trans_buf_set_type(tp, agfl_bp, XFS_BLFT_AGFL_BUF);
		xfs_trans_log_buf(tp, agfl_bp, 0, BBTOB(agfl_bp->b_length) - 1);

		xfs_trans_bhold(tp, sc->sa.agi_bp);
		xfs_trans_bhold(tp, sc->sa.agf_bp);
		xfs_trans_bhold(tp, sc->sa.agfl_bp);

		error = xfs_defer_finish(&sc->tp);
		if (error)
			return error;

		args->tp = sc->tp;
	}

	return xfs_alloc_vextent(args);
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
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_perag	*pag = sc->sa.pag;
	struct xfs_agf		*agf = sc->sa.agf_bp->b_addr;
	unsigned int		old_level;
	int			error;

	/*
	 * Preserve the old rmapbt block count so that we can adjust the
	 * per-AG rmapbt reservation after we commit the new btree root and
	 * want to dispose of the old btree blocks.
	 */
	rr->old_rmapbt_fsbcount = be32_to_cpu(agf->agf_rmap_blocks);

	rr->rmap_bload.get_record = xrep_rmap_get_record;
	rr->rmap_bload.claim_block = xrep_rmap_claim_block;
	xrep_bload_estimate_slack(sc, &rr->rmap_bload);

	/*
	 * Prepare to construct the new btree by reserving disk space for the
	 * new btree and setting up all the accounting information we'll need
	 * to root the new btree while it's under construction and before we
	 * attach it to the AG header.  The new blocks are accounted to the
	 * rmapbt per-AG reservation, which we will adjust further after
	 * committing the new btree.
	 */
	xrep_newbt_init_ag(&rr->new_btree_info, sc, &XFS_RMAP_OINFO_SKIP_UPDATE,
			XFS_AGB_TO_FSB(sc->mp, pag->pag_agno,
				       XFS_RMAP_BLOCK(sc->mp)),
			XFS_AG_RESV_RMAPBT);
	rr->new_btree_info.alloc_vextent = xrep_rmap_alloc_vextent;
	rr->cur = xfs_rmapbt_stage_cursor(sc->mp, &rr->new_btree_info.afake,
			pag);

	/*
	 * Initialize @rr->new_btree_info, reserve space for the new rmapbt,
	 * and compute OWN_AG rmaps.
	 */
	error = xrep_rmap_reserve_space(rr);
	if (error)
		goto err_cur;

	/*
	 * Sort the rmap records by startblock or else the btree records
	 * will be in the wrong order.
	 */
	error = xfarray_sort(rr->rmap_records, xrep_rmap_extent_cmp);
	if (error)
		goto err_cur;

	/*
	 * Due to btree slack factors, it's possible for a new btree to be one
	 * level taller than the old btree.  Update the incore btree height so
	 * that we don't trip the verifiers when writing the new btree blocks
	 * to disk.
	 */
	old_level = pag->pagf_levels[XFS_BTNUM_RMAPi];
	pag->pagf_levels[XFS_BTNUM_RMAPi] = rr->rmap_bload.btree_height;

	/* Add all observed rmap records. */
	rr->iter = 0;
	sc->sa.bno_cur = xfs_allocbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.pag, XFS_BTNUM_BNO);
	error = xfs_btree_bload(rr->cur, &rr->rmap_bload, rr);
	xfs_btree_del_cursor(sc->sa.bno_cur, error);
	sc->sa.bno_cur = NULL;
	if (error)
		goto err_level;

	/*
	 * Install the new btree in the AG header.  After this point the old
	 * btree is no longer accessible and the new tree is live.
	 *
	 * Note: We re-read the AGF here to ensure the buffer type is set
	 * properly.  Since we built a new tree without attaching to the AGF
	 * buffer, the buffer item may have fallen off the buffer.  This ought
	 * to succeed since the AGF is held across transaction rolls.
	 */
	error = xfs_read_agf(sc->mp, sc->tp, pag->pag_agno, 0, &sc->sa.agf_bp);
	if (error)
		goto err_level;

	/* Commit our new btree. */
	xfs_rmapbt_commit_staged_btree(rr->cur, sc->tp, sc->sa.agf_bp);
	xfs_btree_del_cursor(rr->cur, 0);

	/* Reset the AGF counters now that we've changed the btree shape. */
	error = xrep_rmap_reset_counters(rr);
	if (error)
		goto err_newbt;

	/* Dispose of any unused blocks and the accounting information. */
	xrep_newbt_destroy(&rr->new_btree_info, error);

	return xrep_roll_ag_trans(sc);

err_level:
	pag->pagf_levels[XFS_BTNUM_RMAPi] = old_level;
err_cur:
	xfs_btree_del_cursor(rr->cur, error);
err_newbt:
	xrep_newbt_destroy(&rr->new_btree_info, error);
	return error;
}

/* Section (IV): Reaping the old btree. */

/* Subtract each free extent in the bnobt from the rmap gaps. */
STATIC int
xrep_rmap_find_freesp(
	struct xfs_btree_cur		*cur,
	const struct xfs_alloc_rec_incore *rec,
	void				*priv)
{
	struct xbitmap			*bitmap = priv;
	xfs_fsblock_t			fsb;

	fsb = XFS_AGB_TO_FSB(cur->bc_mp, cur->bc_ag.pag->pag_agno,
			rec->ar_startblock);
	return xbitmap_clear(bitmap, fsb, rec->ar_blockcount);
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
	struct xfs_agf		*agf = sc->sa.agf_bp->b_addr;
	struct xfs_btree_cur	*cur;
	xfs_fsblock_t		next_fsb;
	xfs_fsblock_t		agend_fsb;
	uint64_t		nr_records = xfarray_length(rr->rmap_records);
	int			error;

	next_fsb = XFS_AGB_TO_FSB(mp, sc->sa.pag->pag_agno, 0);
	xbitmap_init(&rmap_gaps);

	/* Compute free space from the new rmapbt. */
	for (rr->iter = 0; rr->iter < nr_records; rr->iter++) {
		struct xrep_rmap_extent	rec;
		xfs_fsblock_t	fsbno;

		error = xfarray_load(rr->rmap_records, rr->iter, &rec);
		if (error)
			goto out_bitmap;

		/* Record the free space we find. */
		fsbno = XFS_AGB_TO_FSB(mp, sc->sa.pag->pag_agno,
				rec.startblock);
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
	agend_fsb = XFS_AGB_TO_FSB(mp, sc->sa.pag->pag_agno,
			be32_to_cpu(agf->agf_length));
	if (next_fsb < agend_fsb) {
		error = xbitmap_set(&rmap_gaps, next_fsb,
				agend_fsb - next_fsb);
		if (error)
			goto out_bitmap;
	}

	/* Compute free space from the existing bnobt. */
	cur = xfs_allocbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.pag, XFS_BTNUM_BNO);
	error = xfs_alloc_query_all(cur, xrep_rmap_find_freesp, &rmap_gaps);
	xfs_btree_del_cursor(cur, error);
	if (error)
		goto out_bitmap;

	/*
	 * Free the "free" blocks that the new rmapbt knows about but the bnobt
	 * doesn't--these are the old rmapbt blocks.  Credit the old rmapbt
	 * block usage count back to the per-AG rmapbt reservation (and not
	 * fdblocks, since the rmap btree lives in free space) to keep the
	 * reservation and free space accounting correct.
	 */
	error = xrep_reap_extents(sc, &rmap_gaps, &XFS_RMAP_OINFO_ANY_OWNER,
			XFS_AG_RESV_IGNORE);
	if (error)
		goto out_bitmap;
	sc->sa.pag->pag_rmapbt_resv.ar_reserved += rr->old_rmapbt_fsbcount;

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

	/* Functionality is not yet complete. */
	return xrep_notsupported(sc);

	rr = kmem_zalloc(sizeof(struct xrep_rmap), KM_NOFS | KM_MAYFAIL);
	if (!rr)
		return -ENOMEM;
	rr->sc = sc;

	/* Set up some storage */
	error = xfarray_create(sc->mp, "rmap records",
			sizeof(struct xrep_rmap_extent), &rr->rmap_records);
	if (error)
		goto out_rr;

	rr->iscan.iget_tries = 20;
	rr->iscan.iget_retry_delay = HZ / 10;
	xchk_iscan_start(&rr->iscan);

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
	xchk_iscan_finish(&rr->iscan);
	xfarray_destroy(rr->rmap_records);
out_rr:
	kmem_free(rr);
	return error;
}
