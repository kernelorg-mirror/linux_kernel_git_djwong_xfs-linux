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
#include "xfs_ialloc.h"
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_refcount.h"
#include "xfs_rtrefcount_btree.h"
#include "xfs_error.h"
#include "xfs_health.h"
#include "xfs_inode.h"
#include "xfs_quota.h"
#include "xfs_rtalloc.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/bitmap.h"
#include "scrub/array.h"

/*
 * Rebuilding the Reference Count Btree
 * ====================================
 *
 * This algorithm is "borrowed" from xfs_repair.  Imagine the rmap
 * entries as rectangles representing extents of physical blocks, and
 * that the rectangles can be laid down to allow them to overlap each
 * other; then we know that we must emit a refcnt btree entry wherever
 * the amount of overlap changes, i.e. the emission stimulus is
 * level-triggered:
 *
 *                 -    ---
 *       --      ----- ----   ---        ------
 * --   ----     ----------- ----     ---------
 * -------------------------------- -----------
 * ^ ^  ^^ ^^    ^ ^^ ^^^  ^^^^  ^ ^^ ^  ^     ^
 * 2 1  23 21    3 43 234  2123  1 01 2  3     0
 *
 * For our purposes, a rmap is a tuple (startblock, len, fileoff, owner).
 *
 * Note that in the actual refcnt btree we don't store the refcount < 2
 * cases because the bnobt tells us which blocks are free; single-use
 * blocks aren't recorded in the bnobt or the refcntbt.  If the rmapbt
 * supports storing multiple entries covering a given block we could
 * theoretically dispense with the refcntbt and simply count rmaps, but
 * that's inefficient in the (hot) write path, so we'll take the cost of
 * the extra tree to save time.  Also there's no guarantee that rmap
 * will be enabled.
 *
 * Given an array of rmaps sorted by physical block number, a starting
 * physical block (sp), a bag to hold rmaps that cover sp, and the next
 * physical block where the level changes (np), we can reconstruct the
 * rt refcount btree as follows:
 *
 * While there are still unprocessed rmaps in the array,
 *  - Set sp to the physical block (pblk) of the next unprocessed rmap.
 *  - Add to the bag all rmaps in the array where startblock == sp.
 *  - Set np to the physical block where the bag size will change.  This
 *    is the minimum of (the pblk of the next unprocessed rmap) and
 *    (startblock + len of each rmap in the bag).
 *  - Record the bag size as old_bag_size.
 *
 *  - While the bag isn't empty,
 *     - Remove from the bag all rmaps where startblock + len == np.
 *     - Add to the bag all rmaps in the array where startblock == np.
 *     - If the bag size isn't old_bag_size, store the refcount entry
 *       (sp, np - sp, bag_size) in the refcnt btree.
 *     - If the bag is empty, break out of the inner loop.
 *     - Set old_bag_size to the bag size
 *     - Set sp = np.
 *     - Set np to the physical block where the bag size will change.
 *       This is the minimum of (the pblk of the next unprocessed rmap)
 *       and (startblock + len of each rmap in the bag).
 *
 * Like all the other repairers, we make a list of all the refcount
 * records we need, then reinitialize the rt refcount btree root and
 * insert all the records.
 */

/* The only parts of the rmap that we care about for computing refcounts. */
struct xrep_rtrefc_rmap {
	xfs_rtblock_t		startblock;
	xfs_filblks_t		blockcount;
} __packed;

struct xrep_rtrefc {
	/* refcount extents */
	struct xfbma		*refcount_records;

	/* new refcountbt information */
	struct xrep_newbt	new_btree_info;
	struct xfs_btree_bload	rtrefc_bload;

	/* old refcountbt blocks */
	struct xbitmap		old_rtrefcountbt_blocks;

	struct xfs_scrub	*sc;

	/* # of refcountbt blocks */
	xfs_filblks_t		btblocks;

	/* get_record()'s position in the free space record array. */
	uint64_t		iter;
};

/* Check for any obvious conflicts with this shared/CoW staging extent. */
STATIC int
xrep_rtrefc_check_ext(
	struct xfs_scrub	*sc,
	const struct xfs_refcount_irec	*rec)
{
	/* Must be within the AG and not static data. */
	if (!xfs_verify_rtext(sc->mp, rec->rc_startblock, rec->rc_blockcount))
		return -EFSCORRUPTED;

	/* Make sure this isn't free space or misaligned. */
	return xrep_rtext_is_free(sc, rec->rc_startblock, rec->rc_blockcount,
			true);
}

/* Record a reference count extent. */
STATIC int
xrep_rtrefc_stash(
	struct xrep_rtrefc		*rr,
	xfs_rtblock_t			bno,
	xfs_filblks_t			len,
	xfs_nlink_t			refcount)
{
	struct xfs_refcount_irec	irec = {
		.rc_startblock		= bno,
		.rc_blockcount		= len,
		.rc_refcount		= refcount,
	};
	struct xfs_mount		*mp = rr->sc->mp;
	int				error = 0;

	if (xchk_should_terminate(rr->sc, &error))
		return error;

	error = xrep_rtrefc_check_ext(rr->sc, &irec);
	if (error)
		return error;

	trace_xrep_rtrefc_found(mp, &irec);

	return xfbma_append(rr->refcount_records, &irec);
}

/* Record a CoW staging extent. */
STATIC int
xrep_rtrefc_stash_cow(
	struct xrep_rtrefc		*rr,
	xfs_rtblock_t			bno,
	xfs_filblks_t			len)
{
	return xrep_rtrefc_stash(rr, bno + XFS_RTREFC_COW_START, len, 1);
}

/* Grab the next (abbreviated) rmap record from the rmapbt. */
STATIC int
xrep_rtrefc_next_rrm(
	struct xfs_btree_cur	*cur,
	struct xrep_rtrefc	*rr,
	struct xrep_rtrefc_rmap	*rrm,
	bool			*have_rec)
{
	struct xfs_rmap_irec	rmap;
	struct xfs_mount	*mp = cur->bc_mp;
	int			have_gt;
	int			error = 0;

	*have_rec = false;
	/*
	 * Loop through the remaining rmaps.  Remember CoW staging
	 * extents and the refcountbt blocks from the old tree for later
	 * disposal.  We can only share written data fork extents, so
	 * keep looping until we find an rmap for one.
	 */
	do {
		if (xchk_should_terminate(rr->sc, &error))
			goto out_error;

		error = xfs_btree_increment(cur, 0, &have_gt);
		if (error)
			goto out_error;
		if (!have_gt)
			return 0;

		error = xfs_rmap_get_rec(cur, &rmap, &have_gt);
		if (error)
			goto out_error;
		if (XFS_IS_CORRUPT(mp, !have_gt)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_error;
		}

		if (rmap.rm_owner == XFS_RMAP_OWN_COW) {
			error = xrep_rtrefc_stash_cow(rr, rmap.rm_startblock,
					rmap.rm_blockcount);
			if (error)
				goto out_error;
		}
	} while (XFS_RMAP_NON_INODE_OWNER(rmap.rm_owner) ||
		 xfs_internal_inum(mp, rmap.rm_owner) ||
		 (rmap.rm_flags & (XFS_RMAP_ATTR_FORK | XFS_RMAP_BMBT_BLOCK |
				   XFS_RMAP_UNWRITTEN)));

	rrm->startblock = rmap.rm_startblock;
	rrm->blockcount = rmap.rm_blockcount;
	*have_rec = true;
	return 0;

out_error:
	return error;
}

/* Compare two btree extents. */
static int
xrep_rtrefc_extent_cmp(
	const void			*a,
	const void			*b)
{
	const struct xfs_refcount_irec	*ap = a;
	const struct xfs_refcount_irec	*bp = b;

	if (ap->rc_startblock > bp->rc_startblock)
		return 1;
	else if (ap->rc_startblock < bp->rc_startblock)
		return -1;
	return 0;
}

#define RRM_NEXT(r)	((r).startblock + (r).blockcount)
/*
 * Find the next block where the refcount changes, given the next rmap we
 * looked at and the ones we're already tracking.
 */
static inline xfs_rtblock_t
xrep_rtrefc_next_edge(
	struct xfbma		*rmap_bag,
	struct xrep_rtrefc_rmap	*next_rrm,
	bool			next_valid)
{
	struct xrep_rtrefc_rmap	rrm;
	uint64_t		i;
	xfs_rtblock_t		nbno;

	nbno = next_valid ? next_rrm->startblock : NULLFSBLOCK;
	foreach_xfbma_item(rmap_bag, i, rrm)
		nbno = min_t(xfs_rtblock_t, nbno, RRM_NEXT(rrm));
	return nbno;
}

/* Record extents that belong to the realtime refcount inode. */
STATIC int
xrep_rtrefc_walk_rmap(
	struct xfs_btree_cur	*cur,
	struct xfs_rmap_irec	*rec,
	void			*priv)
{
	struct xrep_rtrefc	*rr = priv;
	struct xfs_mount	*mp = cur->bc_mp;
	xfs_fsblock_t		fsbno;
	int			error = 0;

	if (xchk_should_terminate(rr->sc, &error))
		return error;

	/* Skip extents which are not owned by this inode and fork. */
	if (rec->rm_owner != mp->m_rrefcountip->i_ino)
		return 0;

	error = xrep_check_ino_btree_mapping(rr->sc, rec);
	if (error)
		return error;

	fsbno = XFS_AGB_TO_FSB(mp, cur->bc_ag.agno, rec->rm_startblock);

	return xbitmap_set(&rr->old_rtrefcountbt_blocks, fsbno,
			rec->rm_blockcount);
}

/* Scan one AG for reverse mappings for the realtime refcount btree. */
STATIC int
xrep_rtrefc_scan_ag(
	struct xrep_rtrefc	*rr,
	xfs_agnumber_t		agno)
{
	struct xfs_scrub	*sc = rr->sc;
	int			error;

	error = xrep_ag_init(sc, agno, &sc->sa);
	if (error)
		return error;

	error = xfs_rmap_query_all(sc->sa.rmap_cur, xrep_rtrefc_walk_rmap, rr);
	xchk_ag_free(sc, &sc->sa);
	return error;
}

/* Iterate all the rmap records to generate reference count data. */
STATIC int
xrep_rtrefc_find_refcounts(
	struct xrep_rtrefc	*rr)
{
	struct xrep_rtrefc_rmap	rrm;
	struct xfs_scrub	*sc = rr->sc;
	struct xfbma		*rmap_bag;
	xfs_rtblock_t		sbno;
	xfs_rtblock_t		cbno;
	xfs_rtblock_t		nbno;
	size_t			old_stack_sz;
	size_t			stack_sz = 0;
	xfs_agnumber_t		agno;
	bool			have;
	int			have_gt;
	int			error;

	/* Scan for old rtrefc btree blocks. */
	for (agno = 0; agno < sc->mp->m_sb.sb_agcount; agno++) {
		error = xrep_rtrefc_scan_ag(rr, agno);
		if (error)
			return error;
	}

	xrep_rt_btcur_init(sc, &sc->sr);

	/* Set up some storage */
	rmap_bag = xfbma_init("rtrmap bag", sizeof(struct xrep_rtrefc_rmap));
	if (IS_ERR(rmap_bag)) {
		error = PTR_ERR(rmap_bag);
		goto out_cur;
	}

	/* Start the rtrmapbt cursor to the left of all records. */
	error = xfs_rmap_lookup_le(sc->sr.rmap_cur, 0, 0, 0, 0, 0, &have_gt);
	if (error)
		goto out_bag;
	ASSERT(have_gt == 0);

	/* Process reverse mappings into refcount data. */
	while (xfs_btree_has_more_records(sc->sr.rmap_cur)) {
		/* Push all rmaps with pblk == sbno onto the stack */
		error = xrep_rtrefc_next_rrm(sc->sr.rmap_cur, rr, &rrm, &have);
		if (error)
			goto out_bag;
		if (!have)
			break;
		sbno = cbno = rrm.startblock;
		while (have && rrm.startblock == sbno) {
			error = xfbma_insert_anywhere(rmap_bag, &rrm);
			if (error)
				goto out_bag;
			stack_sz++;
			error = xrep_rtrefc_next_rrm(sc->sr.rmap_cur, rr, &rrm,
					&have);
			if (error)
				goto out_bag;
		}
		error = xfs_btree_decrement(sc->sr.rmap_cur, 0, &have_gt);
		if (error)
			goto out_bag;
		if (XFS_IS_CORRUPT(sc->mp, !have_gt)) {
			xfs_btree_mark_sick(sc->sr.rmap_cur);
			error = -EFSCORRUPTED;
			goto out_bag;
		}

		/* Set nbno to the bno of the next refcount change */
		nbno = xrep_rtrefc_next_edge(rmap_bag, &rrm, have);
		if (nbno == NULLFSBLOCK) {
			error = -EFSCORRUPTED;
			goto out_bag;
		}

		ASSERT(nbno > sbno);
		old_stack_sz = stack_sz;

		/* While stack isn't empty... */
		while (stack_sz) {
			uint64_t	i;

			/* Pop all rmaps that end at nbno */
			foreach_xfbma_item(rmap_bag, i, rrm) {
				if (RRM_NEXT(rrm) != nbno)
					continue;
				error = xfbma_nullify(rmap_bag, i);
				if (error)
					goto out_bag;
				stack_sz--;
			}

			/* Push array items that start at nbno */
			error = xrep_rtrefc_next_rrm(sc->sr.rmap_cur, rr, &rrm,
					&have);
			if (error)
				goto out_bag;
			while (have && rrm.startblock == nbno) {
				error = xfbma_insert_anywhere(rmap_bag,
						&rrm);
				if (error)
					goto out_bag;
				stack_sz++;
				error = xrep_rtrefc_next_rrm(sc->sr.rmap_cur,
						rr, &rrm, &have);
				if (error)
					goto out_bag;
			}
			error = xfs_btree_decrement(sc->sr.rmap_cur, 0,
					&have_gt);
			if (error)
				goto out_bag;
			if (XFS_IS_CORRUPT(sc->mp, !have_gt)) {
				xfs_btree_mark_sick(sc->sr.rmap_cur);
				error = -EFSCORRUPTED;
				goto out_bag;
			}

			/* Emit refcount if necessary */
			ASSERT(nbno > cbno);
			if (stack_sz != old_stack_sz) {
				if (old_stack_sz > 1) {
					error = xrep_rtrefc_stash(rr, cbno,
							nbno - cbno,
							old_stack_sz);
					if (error)
						goto out_bag;
				}
				cbno = nbno;
			}

			/* Stack empty, go find the next rmap */
			if (stack_sz == 0)
				break;
			old_stack_sz = stack_sz;
			sbno = nbno;

			/* Set nbno to the bno of the next refcount change */
			nbno = xrep_rtrefc_next_edge(rmap_bag, &rrm, have);
			if (nbno == NULLFSBLOCK) {
				error = -EFSCORRUPTED;
				goto out_bag;
			}

			ASSERT(nbno > sbno);
		}
	}

	ASSERT(stack_sz == 0);
out_bag:
	xfbma_destroy(rmap_bag);
out_cur:
	xchk_rt_btcur_free(&sc->sr);
	return error;
}
#undef RRM_NEXT

/* Retrieve refcountbt data for bulk load. */
STATIC int
xrep_rtrefc_get_record(
	struct xfs_btree_cur		*cur,
	void				*priv)
{
	struct xrep_rtrefc		*rr = priv;

	return xfbma_iter_get(rr->refcount_records, &rr->iter,
			&cur->bc_rec.rc);
}

/* Feed one of the new btree blocks to the bulk loader. */
STATIC int
xrep_rtrefc_claim_block(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	void			*priv)
{
	struct xrep_rtrefc        *rr = priv;
	int			error;

	error = xrep_newbt_relog_efis(&rr->new_btree_info);
	if (error)
		return error;

	return xrep_newbt_claim_block(cur, &rr->new_btree_info, ptr);
}

/* Update the AGF counters. */
STATIC int
xrep_rtrefc_reset_counters(
	struct xrep_rtrefc	*rr)
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
	delta = ifake->if_blocks - mp->m_rrefcountip->i_d.di_nblocks;
	mp->m_rrefcountip->i_d.di_nblocks = ifake->if_blocks;
	xfs_trans_log_inode(sc->tp, mp->m_rrefcountip, XFS_ILOG_CORE);

	/*
	 * Adjust the quota counts by the difference in size between the old
	 * and new bmbt.
	 */
	if (delta == 0 || !XFS_IS_QUOTA_ON(sc->mp))
		return 0;

	error = xrep_ino_dqattach(sc);
	if (error)
		return error;

	xfs_trans_mod_dquot_byino(sc->tp, mp->m_rrefcountip,
			XFS_TRANS_DQ_BCOUNT, delta);
	return 0;
}

/* Figure out how much space we need to create the incore btree root block. */
STATIC size_t
xrep_rtrefc_iroot_size(
	struct xfs_btree_cur	*cur,
	unsigned int		level,
	unsigned int		nr_this_level,
	void			*priv)
{
	return xfs_rtrefcount_broot_space_calc(cur->bc_mp, level,
			nr_this_level);
}

/*
 * Use the collected refcount information to stage a new rt refcount btree.  If
 * this is successful we'll return with the new btree root information logged
 * to the repair transaction but not yet committed.
 */
STATIC int
xrep_rtrefc_build_new_tree(
	struct xrep_rtrefc	*rr)
{
	struct xfs_owner_info	oinfo;
	struct xfs_scrub	*sc = rr->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_btree_cur	*refc_cur;
	int			error;

	rr->rtrefc_bload.get_record = xrep_rtrefc_get_record;
	rr->rtrefc_bload.claim_block = xrep_rtrefc_claim_block;
	rr->rtrefc_bload.iroot_size = xrep_rtrefc_iroot_size;
	xrep_bload_estimate_slack(sc, &rr->rtrefc_bload);

	/*
	 * Sort the refcount extents by startblock or else the btree records
	 * will be in the wrong order.
	 */
	error = xfbma_sort(rr->refcount_records, xrep_rtrefc_extent_cmp);
	if (error)
		return error;

	/*
	 * Prepare to construct the new btree by reserving disk space for the
	 * new btree and setting up all the accounting information we'll need
	 * to root the new btree while it's under construction and before we
	 * attach it to the realtime refcount inode.
	 */
	xfs_rmap_ino_bmbt_owner(&oinfo, mp->m_rrefcountip->i_ino,
			XFS_DATA_FORK);
	xrep_newbt_init_inode(&rr->new_btree_info, sc, XFS_DATA_FORK, &oinfo);
	refc_cur = xfs_rtrefcountbt_stage_cursor(mp, mp->m_rrefcountip,
			&rr->new_btree_info.ifake);

	/* Compute how many blocks we'll need. */
	error = xfs_btree_bload_compute_geometry(refc_cur, &rr->rtrefc_bload,
			xfbma_length(rr->refcount_records));
	if (error)
		goto err_cur;

	/*
	 * Guess how many blocks we're going to need to rebuild an entire
	 * rtrefcountbt from the number of extents we found, and pump up our
	 * transaction to have sufficient block reservation.
	 */
	error = xfs_trans_reserve_more(sc->tp, rr->rtrefc_bload.nr_blocks, 0);
	if (error)
		goto err_cur;

	/* Reserve the space we'll need for the new btree. */
	error = xrep_newbt_alloc_blocks(&rr->new_btree_info,
			rr->rtrefc_bload.nr_blocks);
	if (error)
		goto err_cur;

	/* Add all observed refcount records. */
	rr->new_btree_info.ifake.if_fork->if_format = XFS_DINODE_FMT_REFCOUNT;
	rr->iter = 0;
	error = xfs_btree_bload(refc_cur, &rr->rtrefc_bload, rr);
	if (error)
		goto err_cur;

	/*
	 * Install the new rtrefc btree in the inode.  After this point the old
	 * btree is no longer accessible and the new tree is live and we can
	 * delete the cursor.
	 */
	xfs_rtrefcountbt_commit_staged_btree(refc_cur, sc->tp);
	xfs_btree_del_cursor(refc_cur, 0);

	/* Reset the AGF counters now that we've changed the btree shape. */
	error = xrep_rtrefc_reset_counters(rr);
	if (error)
		goto err_newbt;

	/* Dispose of any unused blocks and the accounting information. */
	xrep_newbt_destroy(&rr->new_btree_info, error);
	return xrep_roll_trans(sc);
err_cur:
	xfs_btree_del_cursor(refc_cur, error);
err_newbt:
	xrep_newbt_destroy(&rr->new_btree_info, error);
	return error;
}

/*
 * Now that we've logged the roots of the new btrees, invalidate all of the
 * old blocks and free them.
 */
STATIC int
xrep_rtrefc_remove_old_tree(
	struct xrep_rtrefc	*rr)
{
	/* Free the old refcountbt blocks if they're not in use. */
	return xrep_reap_extents(rr->sc, &rr->old_rtrefcountbt_blocks,
			&XFS_RMAP_OINFO_ANY_OWNER, XFS_AG_RESV_RTMETADATA);
}

/* Rebuild the rt refcount btree. */
int
xrep_rtrefcountbt(
	struct xfs_scrub	*sc)
{
	struct xrep_rtrefc	*rr;
	struct xfs_mount	*mp = sc->mp;
	int			error;

	/* We require the rmapbt to rebuild anything. */
	if (!xfs_sb_version_hasrtrmapbt(&mp->m_sb))
		return -EOPNOTSUPP;

	rr = kmem_zalloc(sizeof(struct xrep_rtrefc), KM_NOFS | KM_MAYFAIL);
	if (!rr)
		return -ENOMEM;
	rr->sc = sc;

	xchk_perag_get(sc->mp, &sc->sa);

	/* Set up some storage */
	rr->refcount_records = xfbma_init("rtrefcount records",
			sizeof(struct xfs_refcount_irec));
	if (IS_ERR(rr->refcount_records)) {
		error = PTR_ERR(rr->refcount_records);
		goto out_rr;
	}

	/* Collect all reference counts. */
	xbitmap_init(&rr->old_rtrefcountbt_blocks);
	error = xrep_rtrefc_find_refcounts(rr);
	if (error)
		goto out_bitmap;

	/* Rebuild the refcount information. */
	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	error = xrep_rtrefc_build_new_tree(rr);
	if (error)
		goto out_bitmap;

	/* Kill the old tree. */
	error = xrep_rtrefc_remove_old_tree(rr);

out_bitmap:
	xbitmap_destroy(&rr->old_rtrefcountbt_blocks);
	xfbma_destroy(rr->refcount_records);
out_rr:
	kmem_free(rr);
	return error;
}
