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
 *
 * So basically we scan all primary per-AG metadata and all block maps of all
 * inodes to generate a huge list of reverse map records.  Next we look for
 * gaps in the rmap records to calculate all the unclaimed free space (1).
 * Next, we scan all other OWN_AG metadata (bnobt, cntbt, agfl) and subtract
 * the space used by those btrees from (1), and also subtract the free space
 * listed in the bnobt from (1).  What's left are the gaps in assigned space
 * that the new rmapbt knows about but the existing bnobt doesn't; these are
 * the blocks from the old rmapbt and they can be freed.
 *
 * We use the 'xrep_rmbt' prefix for all the rmap functions.
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
struct xrep_rmbt_extent {
	xfs_agblock_t	startblock;
	xfs_extlen_t	blockcount;
	uint64_t	owner;
	uint64_t	offset;
} __packed;

/* Context for collecting rmaps */
struct xrep_rmbt {
	/* Bitmap of inobt blocks, for generating rmaps later. */
	struct xbitmap		inobt_blocks;

	/* New rmap records generated from primary metadata. */
	struct xfbma		*rmap_records;

	struct xfs_scrub	*sc;

	/*
	 * rmap owner for whatever we're iterating to generate new rmap
	 * records.
	 */
	uint64_t		owner;

	/* New AGF btreeblks value, which won't include old rmapbt blocks. */
	xfs_agblock_t		btblocks;

	/* Number of new rmap records. */
	uint64_t		nr_records;
};

/* Context for calculating old rmapbt blocks */
struct xrep_rmbt_freesp {
	/* Unclaimed (free) space, according to the new rmap. */
	struct xbitmap		rmap_freelist;

	/* Free space accounted for by the free space btrees. */
	struct xbitmap		bno_freelist;

	struct xfs_scrub	*sc;

	/*
	 * Next block we expect to find while scanning the new rmap for
	 * claimed space.
	 */
	xfs_agblock_t		next_bno;
};

/* Initialize an rmap. */
static inline int
xrep_rmbt_new_rec(
	struct xrep_rmbt	*rr,
	xfs_agblock_t		startblock,
	xfs_extlen_t		blockcount,
	uint64_t		owner,
	uint64_t		offset,
	unsigned int		flags)
{
	struct xrep_rmbt_extent	rre = {
		.startblock	= startblock,
		.blockcount	= blockcount,
		.owner		= owner,
	};
	struct xfs_rmap_irec	rmap = {
		.rm_offset	= offset,
		.rm_flags	= flags,
	};
	int			error = 0;

	trace_xrep_rmap_extent_fn(rr->sc->mp, rr->sc->sa.agno, startblock,
			blockcount, owner, offset, flags);

	if (xchk_should_terminate(rr->sc, &error))
		return error;

	rre.offset = xfs_rmap_irec_offset_pack(&rmap);
	return xfbma_append(rr->rmap_records, &rre);
}

/* Add an AGFL block to the rmap list. */
STATIC int
xrep_rmbt_walk_agfl(
	struct xfs_mount	*mp,
	xfs_agblock_t		bno,
	void			*priv)
{
	struct xrep_rmbt	*rr = priv;

	return xrep_rmbt_new_rec(rr, bno, 1, XFS_RMAP_OWN_AG, 0, 0);
}

/* Add a btree block to the rmap list. */
STATIC int
xrep_rmbt_visit_btblock(
	struct xfs_btree_cur	*cur,
	int			level,
	void			*priv)
{
	struct xrep_rmbt	*rr = priv;
	struct xfs_buf		*bp;
	xfs_fsblock_t		fsb;

	xfs_btree_get_block(cur, level, &bp);
	if (!bp)
		return 0;

	rr->btblocks++;
	fsb = XFS_DADDR_TO_FSB(cur->bc_mp, bp->b_bn);
	return xrep_rmbt_new_rec(rr, XFS_FSB_TO_AGBNO(cur->bc_mp, fsb), 1,
			rr->owner, 0, 0);
}

/* Record inode btree rmaps. */
STATIC int
xrep_rmbt_walk_inobt(
	struct xfs_btree_cur		*cur,
	union xfs_btree_rec		*rec,
	void				*priv)
{
	struct xfs_inobt_rec_incore	irec;
	struct xrep_rmbt		*rr = priv;
	struct xfs_mount		*mp = cur->bc_mp;
	xfs_agino_t			agino;
	xfs_agino_t			iperhole;
	unsigned int			i;
	int				error;

	/* Record the inobt blocks. */
	error = xbitmap_set_btcur_path(&rr->inobt_blocks, cur);
	if (error)
		return error;

	xfs_inobt_btrec_to_irec(mp, rec, &irec);

	/* Record a non-sparse inode chunk. */
	if (irec.ir_holemask == XFS_INOBT_HOLEMASK_FULL)
		return xrep_rmbt_new_rec(rr,
				XFS_AGINO_TO_AGBNO(mp, irec.ir_startino),
				XFS_INODES_PER_CHUNK / mp->m_sb.sb_inopblock,
				XFS_RMAP_OWN_INODES, 0, 0);

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
		error = xrep_rmbt_new_rec(rr, XFS_AGINO_TO_AGBNO(mp, agino),
				iperhole / mp->m_sb.sb_inopblock,
				XFS_RMAP_OWN_INODES, 0, 0);
		if (error)
			return error;
	}

	return 0;
}

/* Record a CoW staging extent. */
STATIC int
xrep_rmbt_walk_cowblocks(
	struct xfs_btree_cur		*cur,
	union xfs_btree_rec		*rec,
	void				*priv)
{
	struct xrep_rmbt		*rr = priv;
	struct xfs_refcount_irec	refc;

	xfs_refcount_btrec_to_irec(rec, &refc);
	if (refc.rc_refcount != 1)
		return -EFSCORRUPTED;

	return xrep_rmbt_new_rec(rr, refc.rc_startblock - XFS_REFC_COW_START,
			refc.rc_blockcount, XFS_RMAP_OWN_COW, 0, 0);
}

/* Add a bmbt block to the rmap list. */
STATIC int
xrep_rmbt_visit_bmbt(
	struct xfs_btree_cur	*cur,
	int			level,
	void			*priv)
{
	struct xrep_rmbt	*rr = priv;
	struct xfs_buf		*bp;
	xfs_fsblock_t		fsb;
	unsigned int		flags = XFS_RMAP_BMBT_BLOCK;

	xfs_btree_get_block(cur, level, &bp);
	if (!bp)
		return 0;

	fsb = XFS_DADDR_TO_FSB(cur->bc_mp, bp->b_bn);
	if (XFS_FSB_TO_AGNO(cur->bc_mp, fsb) != rr->sc->sa.agno)
		return 0;

	if (cur->bc_private.b.whichfork == XFS_ATTR_FORK)
		flags |= XFS_RMAP_ATTR_FORK;
	return xrep_rmbt_new_rec(rr, XFS_FSB_TO_AGBNO(cur->bc_mp, fsb), 1,
			cur->bc_private.b.ip->i_ino, 0, flags);
}

/* Determine rmap flags from fork and bmbt state. */
static inline unsigned int
xrep_rmbt_bmap_flags(
	int			whichfork,
	xfs_exntst_t		state)
{
	return  (whichfork == XFS_ATTR_FORK ? XFS_RMAP_ATTR_FORK : 0) |
		(state == XFS_EXT_UNWRITTEN ? XFS_RMAP_UNWRITTEN : 0);
}

/* Find all the extents from a given AG in an inode fork. */
STATIC int
xrep_rmbt_scan_ifork(
	struct xrep_rmbt	*rr,
	struct xfs_inode	*ip,
	int			whichfork)
{
	struct xfs_bmbt_irec	rec;
	struct xfs_iext_cursor	icur;
	struct xfs_mount	*mp = rr->sc->mp;
	struct xfs_btree_cur	*cur = NULL;
	struct xfs_ifork	*ifp;
	unsigned int		rflags;
	int			fmt;
	int			error = 0;

	/* Do we even have data mapping extents? */
	fmt = XFS_IFORK_FORMAT(ip, whichfork);
	ifp = XFS_IFORK_PTR(ip, whichfork);
	switch (fmt) {
	case XFS_DINODE_FMT_BTREE:
		if (!(ifp->if_flags & XFS_IFEXTENTS)) {
			error = xfs_iread_extents(rr->sc->tp, ip, whichfork);
			if (error)
				return error;
		}
		break;
	case XFS_DINODE_FMT_EXTENTS:
		break;
	default:
		return 0;
	}
	if (!ifp)
		return 0;

	/* Find all the BMBT blocks in the AG. */
	if (fmt == XFS_DINODE_FMT_BTREE) {
		cur = xfs_bmbt_init_cursor(mp, rr->sc->tp, ip, whichfork);
		error = xfs_btree_visit_blocks(cur, xrep_rmbt_visit_bmbt, rr);
		if (error)
			goto out;
		xfs_btree_del_cursor(cur, error);
		cur = NULL;
	}

	/* We're done if this is an rt inode's data fork. */
	if (whichfork == XFS_DATA_FORK && XFS_IS_REALTIME_INODE(ip))
		return 0;

	/* Find all the extents in the AG. */
	for_each_xfs_iext(ifp, &icur, &rec) {
		if (isnullstartblock(rec.br_startblock))
			continue;
		/* Stash non-hole extent. */
		if (XFS_FSB_TO_AGNO(mp, rec.br_startblock) == rr->sc->sa.agno) {
			rflags = xrep_rmbt_bmap_flags(whichfork, rec.br_state);
			error = xrep_rmbt_new_rec(rr,
					XFS_FSB_TO_AGBNO(mp, rec.br_startblock),
					rec.br_blockcount, ip->i_ino,
					rec.br_startoff, rflags);
			if (error)
				goto out;
		}
	}
out:
	if (cur)
		xfs_btree_del_cursor(cur, error);
	return error;
}

/* Record reverse mappings for a file. */
STATIC int
xrep_rmbt_scan_inode(
	struct xfs_mount		*mp,
	struct xfs_trans		*tp,
	xfs_ino_t			ino,
	void				*data)
{
	struct xrep_rmbt		*rr = data;
	struct xfs_inode		*ip;
	unsigned int			lock_mode;
	int				error;

	/* Grab inode and lock it so we can scan it. */
	error = xfs_iget(mp, rr->sc->tp, ino, XFS_IGET_DONTCACHE, 0, &ip);
	if (error)
		return error;

	lock_mode = xfs_ilock_data_map_shared(ip);

	/* Check the data fork. */
	error = xrep_rmbt_scan_ifork(rr, ip, XFS_DATA_FORK);
	if (error)
		goto out_unlock;

	/* Check the attr fork. */
	error = xrep_rmbt_scan_ifork(rr, ip, XFS_ATTR_FORK);
	if (error)
		goto out_unlock;

	/* COW fork extents are "owned" by the refcount btree. */

out_unlock:
	xfs_iunlock(ip, lock_mode);
	xfs_irele(ip);
	return error;
}

/* Find all the unclaimed space in the new rmap records. */
STATIC int
xrep_rmbt_record_rmap_freesp(
	struct xfs_btree_cur	*cur,
	struct xfs_rmap_irec	*rec,
	void			*priv)
{
	struct xrep_rmbt_freesp	*rrf = priv;
	xfs_fsblock_t		fsb;
	int			error;

	/* Record the free space we find. */
	if (rec->rm_startblock > rrf->next_bno) {
		fsb = XFS_AGB_TO_FSB(cur->bc_mp, cur->bc_private.a.agno,
				rrf->next_bno);
		error = xbitmap_set(&rrf->rmap_freelist, fsb,
				rec->rm_startblock - rrf->next_bno);
		if (error)
			return error;
	}
	rrf->next_bno = max_t(xfs_agblock_t, rrf->next_bno,
			rec->rm_startblock + rec->rm_blockcount);
	return 0;
}

/* Find all the free space recorded in the AG. */
STATIC int
xrep_rmbt_record_bno_freesp(
	struct xfs_btree_cur		*cur,
	struct xfs_alloc_rec_incore	*rec,
	void				*priv)
{
	struct xrep_rmbt_freesp		*rrf = priv;
	xfs_fsblock_t			fsb;

	/* Record the free space we find. */
	fsb = XFS_AGB_TO_FSB(cur->bc_mp, cur->bc_private.a.agno,
			rec->ar_startblock);
	return xbitmap_set(&rrf->bno_freelist, fsb, rec->ar_blockcount);
}

/* Compare two rmapbt extents. */
static int
xrep_rmbt_extent_cmp(
	const void			*a,
	const void			*b)
{
	const struct xrep_rmbt_extent	*ap = a;
	const struct xrep_rmbt_extent	*bp = b;
	struct xfs_rmap_irec		ar = {
		.rm_startblock	= ap->startblock,
		.rm_blockcount	= ap->blockcount,
		.rm_owner	= ap->owner,
	};
	struct xfs_rmap_irec		br = {
		.rm_startblock	= bp->startblock,
		.rm_blockcount	= bp->blockcount,
		.rm_owner	= bp->owner,
	};
	int				error;

	error = xfs_rmap_irec_offset_unpack(ap->offset, &ar);
	ASSERT(error == 0);
	error = xfs_rmap_irec_offset_unpack(bp->offset, &br);
	ASSERT(error == 0);

	return xfs_rmap_compare(&ar, &br);
}

/* Generate rmaps for the AG headers (AGI/AGF/AGFL) */
STATIC int
xrep_rmbt_generate_agheader_rmaps(
	struct xrep_rmbt	*rr)
{
	struct xfs_scrub	*sc = rr->sc;
	int			error;

	/* Create a record for the AG sb->agfl. */
	error = xrep_rmbt_new_rec(rr, XFS_SB_BLOCK(sc->mp),
			XFS_AGFL_BLOCK(sc->mp) - XFS_SB_BLOCK(sc->mp) + 1,
			XFS_RMAP_OWN_FS, 0, 0);
	if (error)
		return error;

	/* Generate rmaps for the blocks in the AGFL. */
	return xfs_agfl_walk(sc->mp, XFS_BUF_TO_AGF(sc->sa.agf_bp),
			sc->sa.agfl_bp, xrep_rmbt_walk_agfl, rr);
}

/* Generate rmaps for the log, if it's in this AG. */
STATIC int
xrep_rmbt_generate_log_rmaps(
	struct xrep_rmbt	*rr)
{
	struct xfs_scrub	*sc = rr->sc;

	if (sc->mp->m_sb.sb_logstart == 0 ||
	    XFS_FSB_TO_AGNO(sc->mp, sc->mp->m_sb.sb_logstart) != sc->sa.agno)
		return 0;

	return xrep_rmbt_new_rec(rr,
			XFS_FSB_TO_AGBNO(sc->mp, sc->mp->m_sb.sb_logstart),
			sc->mp->m_sb.sb_logblocks, XFS_RMAP_OWN_LOG, 0, 0);
}

/* Collect rmaps for the blocks containing the free space btrees. */
STATIC int
xrep_rmbt_generate_freesp_rmaps(
	struct xrep_rmbt	*rr,
	xfs_agblock_t		*new_btreeblks)
{
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_btree_cur	*cur;
	int			error;

	rr->owner = XFS_RMAP_OWN_AG;
	rr->btblocks = 0;

	/* bnobt */
	cur = xfs_allocbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.agno, XFS_BTNUM_BNO);
	error = xfs_btree_visit_blocks(cur, xrep_rmbt_visit_btblock, rr);
	if (error)
		goto err;
	xfs_btree_del_cursor(cur, error);

	/* cntbt */
	cur = xfs_allocbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.agno, XFS_BTNUM_CNT);
	error = xfs_btree_visit_blocks(cur, xrep_rmbt_visit_btblock, rr);
	if (error)
		goto err;
	xfs_btree_del_cursor(cur, error);

	/* btreeblks doesn't include the bnobt/cntbt btree roots */
	*new_btreeblks = rr->btblocks - 2;
	return 0;
err:
	xfs_btree_del_cursor(cur, error);
	return error;
}

/* Collect rmaps for the blocks containing inode btrees and the inode chunks. */
STATIC int
xrep_rmbt_generate_inobt_rmaps(
	struct xrep_rmbt	*rr)
{
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_btree_cur	*cur;
	struct interval_tree_node *itn, *n;
	int			error;

	rr->owner = XFS_RMAP_OWN_INOBT;

	/*
	 * Iterate every record in the inobt so we can capture all the inode
	 * chunks and the blocks in the inobt itself.
	 */
	cur = xfs_inobt_init_cursor(sc->mp, sc->tp, sc->sa.agi_bp,
			sc->sa.agno, XFS_BTNUM_INO);
	error = xfs_btree_query_all(cur, xrep_rmbt_walk_inobt, rr);
	if (error)
		goto err_cur;
	xfs_btree_del_cursor(cur, error);

	/*
	 * Note that if there are zero records in the inobt then query_all does
	 * nothing and we have to account the empty inobt root manually.
	 */
	if (xbitmap_empty(&rr->inobt_blocks)) {
		struct xfs_agi	*agi;
		xfs_fsblock_t	agi_root;

		agi = XFS_BUF_TO_AGI(sc->sa.agi_bp);
		agi_root = XFS_AGB_TO_FSB(sc->mp, sc->sa.agno,
				be32_to_cpu(agi->agi_root));
		xbitmap_set(&rr->inobt_blocks, agi_root, 1);
	}

	/* Add all the inobt blocks to the rmap list. */
	for_each_xbitmap_extent(itn, n, &rr->inobt_blocks) {
		error = xrep_rmbt_new_rec(rr,
				XFS_FSB_TO_AGBNO(sc->mp, itn->start),
				itn->last - itn->start + 1,
				XFS_RMAP_OWN_INOBT, 0, 0);
		if (error)
			goto err;
	}

	/* finobt */
	if (!xfs_sb_version_hasfinobt(&sc->mp->m_sb))
		return 0;

	cur = xfs_inobt_init_cursor(sc->mp, sc->tp, sc->sa.agi_bp, sc->sa.agno,
			XFS_BTNUM_FINO);
	error = xfs_btree_visit_blocks(cur, xrep_rmbt_visit_btblock, rr);
	if (error)
		goto err_cur;
	xfs_btree_del_cursor(cur, error);
	return 0;
err_cur:
	xfs_btree_del_cursor(cur, error);
err:
	return error;
}

/*
 * Collect rmaps for the blocks containing the refcount btree, and all CoW
 * staging extents.
 */
STATIC int
xrep_rmbt_generate_refcountbt_rmaps(
	struct xrep_rmbt	*rr)
{
	union xfs_btree_irec	low;
	union xfs_btree_irec	high;
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_btree_cur	*cur;
	int			error;

	if (!xfs_sb_version_hasreflink(&sc->mp->m_sb))
		return 0;

	rr->owner = XFS_RMAP_OWN_REFC;

	/* refcountbt */
	cur = xfs_refcountbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.agno);
	error = xfs_btree_visit_blocks(cur, xrep_rmbt_visit_btblock, rr);
	if (error)
		goto err_cur;

	/* Collect rmaps for CoW staging extents. */
	memset(&low, 0, sizeof(low));
	low.rc.rc_startblock = XFS_REFC_COW_START;
	memset(&high, 0xFF, sizeof(high));
	error = xfs_btree_query_range(cur, &low, &high,
			xrep_rmbt_walk_cowblocks, rr);
err_cur:
	xfs_btree_del_cursor(cur, error);
	return error;
}

/*
 * Generate all the reverse-mappings for this AG, a list of the old rmapbt
 * blocks, and the new btreeblks count.  Figure out if we have enough free
 * space to reconstruct the inode btrees.  The caller must clean up the lists
 * if anything goes wrong.
 */
STATIC int
xrep_rmbt_find_rmaps(
	struct xfs_scrub	*sc,
	struct xfbma		*rmap_records,
	xfs_agblock_t		*new_btreeblks)
{
	struct xrep_rmbt	rr = {
		.sc		= sc,
		.rmap_records	= rmap_records,
		.nr_records	= 0,
	};
	int			error;

	xbitmap_init(&rr.inobt_blocks);

	/* Generate rmaps for AG space metadata */
	error = xrep_rmbt_generate_agheader_rmaps(&rr);
	if (error)
		return error;
	error = xrep_rmbt_generate_log_rmaps(&rr);
	if (error)
		return error;
	error = xrep_rmbt_generate_freesp_rmaps(&rr, new_btreeblks);
	if (error)
		return error;
	error = xrep_rmbt_generate_inobt_rmaps(&rr);
	if (error)
		return error;
	error = xrep_rmbt_generate_refcountbt_rmaps(&rr);
	if (error)
		return error;

	/* Iterate all AGs for inodes rmaps. */
	error = xfs_iwalk(sc->mp, sc->tp, 0, 0, xrep_rmbt_scan_inode, 0, &rr);
	if (error)
		return error;

	/* Do we actually have enough space to do this? */
	if (!xrep_ag_has_space(sc->sa.pag,
			xfs_rmapbt_calc_size(sc->mp, rr.nr_records),
			XFS_AG_RESV_RMAPBT))
		return -ENOSPC;

	return 0;
}

/* Update the AGF counters. */
STATIC int
xrep_rmbt_reset_counters(
	struct xfs_scrub	*sc,
	xfs_agblock_t		new_btreeblks,
	int			*log_flags)
{
	struct xfs_agf		*agf;
	struct xfs_perag	*pag = sc->sa.pag;

	agf = XFS_BUF_TO_AGF(sc->sa.agf_bp);
	ASSERT(pag->pagf_init);
	pag->pagf_init = 0;
	pag->pagf_btreeblks = new_btreeblks;
	agf->agf_btreeblks = cpu_to_be32(new_btreeblks);
	*log_flags |= XFS_AGF_BTREEBLKS;

	return 0;
}

/* Initialize a new rmapbt root and implant it into the AGF. */
STATIC int
xrep_rmbt_reset_btree(
	struct xfs_scrub	*sc,
	int			*log_flags)
{
	struct xfs_buf		*bp;
	struct xfs_agf		*agf;
	struct xfs_perag	*pag = sc->sa.pag;
	xfs_fsblock_t		btfsb;
	int			error;

	agf = XFS_BUF_TO_AGF(sc->sa.agf_bp);

	/* Initialize a new rmapbt root. */
	error = xrep_alloc_ag_block(sc, &XFS_RMAP_OINFO_SKIP_UPDATE, &btfsb,
			XFS_AG_RESV_RMAPBT);
	if (error)
		return error;

	/* The root block is not a btreeblks block. */
	be32_add_cpu(&agf->agf_btreeblks, -1);
	pag->pagf_btreeblks--;
	*log_flags |= XFS_AGF_BTREEBLKS;

	error = xrep_init_btblock(sc, btfsb, &bp, XFS_BTNUM_RMAP,
			&xfs_rmapbt_buf_ops);
	if (error)
		return error;

	agf->agf_roots[XFS_BTNUM_RMAPi] =
			cpu_to_be32(XFS_FSB_TO_AGBNO(sc->mp, btfsb));
	agf->agf_levels[XFS_BTNUM_RMAPi] = cpu_to_be32(1);
	agf->agf_rmap_blocks = cpu_to_be32(1);
	pag->pagf_levels[XFS_BTNUM_RMAPi] = 1;
	*log_flags |= XFS_AGF_ROOTS | XFS_AGF_LEVELS | XFS_AGF_RMAP_BLOCKS;

	return 0;
}

/*
 * Make our new btree root permanent so that we can start refilling the rmap
 * records.
 */
STATIC int
xrep_rmbt_commit_new(
	struct xfs_scrub	*sc,
	int			log_flags)
{
	int			error;

	xfs_alloc_log_agf(sc->tp, sc->sa.agf_bp, log_flags);
	error = xrep_roll_ag_trans(sc);
	if (error)
		return error;
	sc->sa.pag->pagf_init = 1;
	sc->flags |= XREP_RESET_PERAG_RESV;
	return 0;
}

/*
 * Roll and fix the free list while reloading the rmapbt.  Do not shrink the
 * freelist because the rmapbt is not fully set up yet.
 */
STATIC int
xrep_rmbt_fix_freelist(
	struct xfs_scrub	*sc)
{
	int			error;

	error = xrep_roll_ag_trans(sc);
	if (error)
		return error;
	return xrep_fix_freelist(sc, false);
}

struct xrep_add_rmap {
	struct xfs_scrub	*sc;
	struct xfs_btree_cur	*cur;
	uint32_t		old_rmbt_size;
};

static inline unsigned int
xrep_rmbt_size(
	struct xfs_scrub	*sc)
{
	struct xfs_agf		*agf = XFS_BUF_TO_AGF(sc->sa.agf_bp);

	return be32_to_cpu(agf->agf_rmap_blocks);
}

/* Add one rmap record. */
STATIC int
xrep_rmbt_insert_rec(
	const void			*item,
	void				*priv)
{
	const struct xrep_rmbt_extent	*rre = item;
	struct xfs_rmap_irec		rmap = {
		.rm_startblock		= rre->startblock,
		.rm_blockcount		= rre->blockcount,
		.rm_owner		= rre->owner,
	};
	struct xrep_add_rmap		*x = priv;
	int				error;

	error = xfs_rmap_irec_offset_unpack(rre->offset, &rmap);
	if (error)
		return error;

	/* Add the rmap. */
	error = xfs_rmap_map_raw(x->cur, &rmap);
	if (error)
		return error;

	/*
	 * If the flcount changed because the rmap btree changed shape then we
	 * need to fix the freelist to keep it full enough to handle a total
	 * btree split.  We'll roll this transaction to get it out of the way
	 * and then fix the freelist in a fresh transaction.
	 *
	 * However, two things we must be careful about: (1) fixing the
	 * freelist changes the rmapbt so drop the rmapbt cursor and (2) we
	 * can't let the freelist shrink.  The rmapbt isn't fully set up yet,
	 * which means that the current AGFL blocks might not be reflected in
	 * the rmapbt, which is a problem if we want to unmap blocks from the
	 * AGFL.
	 */
	if (xrep_rmbt_size(x->sc) == x->old_rmbt_size)
		return 0;

	xfs_btree_del_cursor(x->cur, error);
	x->cur = NULL;
	error = xrep_rmbt_fix_freelist(x->sc);
	if (error)
		return error;
	x->old_rmbt_size = xrep_rmbt_size(x->sc);
	x->cur = xfs_rmapbt_init_cursor(x->sc->mp, x->sc->tp, x->sc->sa.agf_bp,
			x->sc->sa.agno);
	return 0;
}

/* Insert all the rmaps we collected. */
STATIC int
xrep_rmbt_rebuild_tree(
	struct xfs_scrub	*sc,
	struct xfbma		*rmap_records)
{
	struct xrep_add_rmap	x = {
		.sc	= sc,
	};
	struct xfs_mount	*mp = sc->mp;
	int			error;

	/*
	 * Sort the reverse mappings by startblock to avoid btree splits when
	 * we rebuild the rmap btree.
	 */
	error = xfbma_sort(rmap_records, xrep_rmbt_extent_cmp);
	if (error)
		return error;

	/* Put everything back in the rmapbt. */
	x.cur = xfs_rmapbt_init_cursor(mp, sc->tp, sc->sa.agf_bp, sc->sa.agno);
	x.old_rmbt_size = xrep_rmbt_size(sc);
	error = xfbma_iter_del(rmap_records, xrep_rmbt_insert_rec, &x);
	if (x.cur)
		xfs_btree_del_cursor(x.cur, error);
	if (error)
		goto err;

	/* Fix the freelist once more, if necessary. */
	if (xrep_rmbt_size(sc) != x.old_rmbt_size) {
		error = xrep_rmbt_fix_freelist(sc);
		if (error)
			goto err;
	}
	return 0;
err:
	return error;
}

/*
 * Reap the old rmapbt blocks.  Now that the rmapbt is fully rebuilt, we make
 * a list of gaps in the rmap records and a list of the extents mentioned in
 * the bnobt.  Any block that's in the new rmapbt gap list but not mentioned
 * in the bnobt is a block from the old rmapbt and can be removed.
 */
STATIC int
xrep_rmbt_reap_old_blocks(
	struct xfs_scrub	*sc)
{
	struct xrep_rmbt_freesp	rrf = {
		.next_bno	= 0,
		.sc		= sc,
	};
	struct xfs_mount	*mp = sc->mp;
	struct xfs_agf		*agf;
	struct xfs_btree_cur	*cur;
	xfs_fsblock_t		btfsb;
	xfs_agblock_t		agend;
	int			error;

	xbitmap_init(&rrf.rmap_freelist);
	xbitmap_init(&rrf.bno_freelist);

	/* Compute free space from the new rmapbt. */
	cur = xfs_rmapbt_init_cursor(mp, sc->tp, sc->sa.agf_bp, sc->sa.agno);
	error = xfs_rmap_query_all(cur, xrep_rmbt_record_rmap_freesp,
			&rrf);
	if (error)
		goto err_cur;
	xfs_btree_del_cursor(cur, error);

	/* Insert a record for space between the last rmap and EOAG. */
	agf = XFS_BUF_TO_AGF(sc->sa.agf_bp);
	agend = be32_to_cpu(agf->agf_length);
	if (rrf.next_bno < agend) {
		btfsb = XFS_AGB_TO_FSB(mp, sc->sa.agno, rrf.next_bno);
		error = xbitmap_set(&rrf.rmap_freelist, btfsb,
				agend - rrf.next_bno);
		if (error)
			goto err;
	}

	/* Compute free space from the existing bnobt. */
	cur = xfs_allocbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.agno, XFS_BTNUM_BNO);
	error = xfs_alloc_query_all(cur, xrep_rmbt_record_bno_freesp, &rrf);
	if (error)
		goto err_lists;
	xfs_btree_del_cursor(cur, error);

	/*
	 * Free the "free" blocks that the new rmapbt knows about but
	 * the old bnobt doesn't.  These are the old rmapbt blocks.
	 */
	xbitmap_disunion(&rrf.rmap_freelist, &rrf.bno_freelist);
	xbitmap_destroy(&rrf.bno_freelist);
	return xrep_reap_extents(sc, &rrf.rmap_freelist,
			&XFS_RMAP_OINFO_ANY_OWNER, XFS_AG_RESV_RMAPBT);
err_lists:
	xbitmap_destroy(&rrf.bno_freelist);
err_cur:
	xfs_btree_del_cursor(cur, error);
err:
	return error;
}

/* Repair the rmap btree for some AG. */
int
xrep_rmapbt(
	struct xfs_scrub	*sc)
{
	struct xfbma		*rmap_records;
	xfs_extlen_t		new_btreeblks;
	int			log_flags = 0;
	int			error;

	xchk_perag_get(sc->mp, &sc->sa);

	/* Set up some storage */
	rmap_records = xfbma_init(sizeof(struct xrep_rmbt_extent));
	if (IS_ERR(rmap_records))
		return PTR_ERR(rmap_records);

	/* Collect rmaps for all AG headers. */
	error = xrep_rmbt_find_rmaps(sc, rmap_records, &new_btreeblks);
	if (error)
		goto out;

	/*
	 * Blow out the old rmap btrees.  This is the point at which
	 * we are no longer able to bail out gracefully.
	 */
	error = xrep_rmbt_reset_counters(sc, new_btreeblks, &log_flags);
	if (error)
		goto out;
	error = xrep_rmbt_reset_btree(sc, &log_flags);
	if (error)
		goto out;
	error = xrep_rmbt_commit_new(sc, log_flags);
	if (error)
		goto out;

	/* Now rebuild the rmap information. */
	error = xrep_rmbt_rebuild_tree(sc, rmap_records);
	if (error)
		goto out;

	/* Find and destroy the blocks from the old rmapbt. */
	error = xrep_rmbt_reap_old_blocks(sc);
	if (error)
		goto out;
out:
	xfbma_destroy(rmap_records);
	return error;
}
