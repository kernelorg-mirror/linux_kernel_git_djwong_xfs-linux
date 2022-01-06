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
#include "xfs_bit.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_alloc.h"
#include "xfs_alloc_btree.h"
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_inode.h"
#include "xfs_refcount.h"
#include "xfs_extent_busy.h"
#include "xfs_health.h"
#include "xfs_bmap.h"
#include "xfs_ialloc.h"
#include "xfs_ag.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/bitmap.h"
#include "scrub/xfarray.h"

/*
 * Free Space Btree Repair
 * =======================
 *
 * The reverse mappings are supposed to record all space usage for the entire
 * AG.  Therefore, we can recalculate the free extents in an AG by looking for
 * gaps in the physical extents recorded in the rmapbt.  On a reflink
 * filesystem this is a little more tricky in that we have to be aware that
 * the rmap records are allowed to overlap.
 *
 * We derive which blocks belonged to the old bnobt/cntbt by recording all the
 * OWN_AG extents and subtracting out the blocks owned by all other OWN_AG
 * metadata: the rmapbt blocks visited while iterating the reverse mappings
 * and the AGFL blocks.
 *
 * Once we have both of those pieces, we can reconstruct the bnobt and cntbt
 * by blowing out the free block state and freeing all the extents that we
 * found.  This adds the requirement that we can't have any busy extents in
 * the AG because the busy code cannot handle duplicate records.
 *
 * Note that we can only rebuild both free space btrees at the same time
 * because the regular extent freeing infrastructure loads both btrees at the
 * same time.
 *
 * We use the prefix 'xrep_abt' here because we regenerate both free space
 * allocation btrees at the same time.
 */

struct xrep_abt {
	/* Blocks owned by the rmapbt or the agfl. */
	struct xbitmap		not_allocbt_blocks;

	/* All OWN_AG blocks. */
	struct xbitmap		old_allocbt_blocks;

	/*
	 * New bnobt information.  All btree block reservations are added to
	 * the reservation list in new_bnobt_info.
	 */
	struct xrep_newbt	new_bnobt_info;
	struct xfs_btree_bload	bno_bload;

	/* new cntbt information */
	struct xrep_newbt	new_cntbt_info;
	struct xfs_btree_bload	cnt_bload;

	/* Free space extents. */
	struct xfarray		*free_records;

	struct xfs_scrub	*sc;

	/* Number of non-null records in @free_records. */
	uint64_t		nr_real_records;

	/* get_records()'s position in the free space record array. */
	xfarray_idx_t		array_cur;

	/*
	 * Next block we anticipate seeing in the rmap records.  If the next
	 * rmap record is greater than next_bno, we have found unused space.
	 */
	xfs_agblock_t		next_bno;

	/* Number of free blocks in this AG. */
	xfs_agblock_t		nr_blocks;

	/* Longest free extent we found in the AG. */
	xfs_agblock_t		longest;
};

/* Set up to repair AG free space btrees. */
int
xrep_setup_ag_allocbt(
	struct xfs_scrub	*sc)
{
	unsigned int		busy_gen;

	/*
	 * Make sure the busy extent list is clear because we can't put extents
	 * on there twice.
	 */
	busy_gen = READ_ONCE(sc->sa.pag->pagb_gen);
	if (!xfs_extent_busy_list_empty(sc->sa.pag))
		xfs_extent_busy_flush(sc->mp, sc->sa.pag, busy_gen);

	return 0;
}

/* Check for any obvious conflicts in the free extent. */
STATIC int
xrep_abt_check_free_ext(
	struct xfs_scrub	*sc,
	const struct xfs_alloc_rec_incore *rec)
{
	enum xfs_btree_keyfill	keyfill;
	int			error;

	/* Must be within the AG and not static data. */
	if (!xfs_verify_agbext(sc->mp, sc->sa.pag->pag_agno, rec->ar_startblock,
				rec->ar_blockcount))
		return -EFSCORRUPTED;

	/* Must not be an inode chunk. */
	error = xfs_ialloc_has_inodes_at_extent(sc->sa.ino_cur,
			rec->ar_startblock, rec->ar_blockcount, &keyfill);
	if (error)
		return error;
	if (keyfill != XFS_BTREE_KEYFILL_EMPTY)
		return -EFSCORRUPTED;

	/* Must not be shared or CoW staging. */
	if (sc->sa.refc_cur) {
		error = xfs_refcount_scan_keyfill(sc->sa.refc_cur,
				rec->ar_startblock, rec->ar_blockcount,
				&keyfill);
		if (error)
			return error;
		if (keyfill != XFS_BTREE_KEYFILL_EMPTY)
			return -EFSCORRUPTED;
	}

	return 0;
}

/*
 * Stash a free space record for all the space since the last bno we found
 * all the way up to @end.
 */
static int
xrep_abt_stash(
	struct xrep_abt		*ra,
	xfs_agblock_t		end)
{
	struct xfs_alloc_rec_incore arec = {
		.ar_startblock	= ra->next_bno,
		.ar_blockcount	= end - ra->next_bno,
	};
	struct xfs_scrub	*sc = ra->sc;
	int			error = 0;

	if (xchk_should_terminate(sc, &error))
		return error;

	error = xrep_abt_check_free_ext(ra->sc, &arec);
	if (error)
		return error;

	trace_xrep_abt_found(sc->mp, sc->sa.pag->pag_agno, &arec);

	error = xfarray_append(ra->free_records, &arec);
	if (error)
		return error;

	ra->nr_blocks += arec.ar_blockcount;
	return 0;
}

/* Record extents that aren't in use from gaps in the rmap records. */
STATIC int
xrep_abt_walk_rmap(
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec,
	void				*priv)
{
	struct xrep_abt			*ra = priv;
	xfs_fsblock_t			fsb;
	int				error;

	/* Record all the OWN_AG blocks... */
	if (rec->rm_owner == XFS_RMAP_OWN_AG) {
		fsb = XFS_AGB_TO_FSB(cur->bc_mp, cur->bc_ag.pag->pag_agno,
				rec->rm_startblock);
		error = xbitmap_set(&ra->old_allocbt_blocks, fsb,
				rec->rm_blockcount);
		if (error)
			return error;
	}

	/* ...and all the rmapbt blocks... */
	error = xbitmap_set_btcur_path(&ra->not_allocbt_blocks, cur);
	if (error)
		return error;

	/* ...and all the free space. */
	if (rec->rm_startblock > ra->next_bno) {
		error = xrep_abt_stash(ra, rec->rm_startblock);
		if (error)
			return error;
	}

	/*
	 * rmap records can overlap on reflink filesystems, so project next_bno
	 * as far out into the AG space as we currently know about.
	 */
	ra->next_bno = max_t(xfs_agblock_t, ra->next_bno,
			rec->rm_startblock + rec->rm_blockcount);
	return 0;
}

/* Collect an AGFL block for the not-to-release list. */
static int
xrep_abt_walk_agfl(
	struct xfs_mount	*mp,
	xfs_agblock_t		bno,
	void			*priv)
{
	struct xrep_abt		*ra = priv;
	xfs_fsblock_t		fsb;

	fsb = XFS_AGB_TO_FSB(mp, ra->sc->sa.pag->pag_agno, bno);
	return xbitmap_set(&ra->not_allocbt_blocks, fsb, 1);
}

/*
 * Compare two free space extents by block number.  We want to sort in order of
 * increasing block number.
 */
static int
xrep_bnobt_extent_cmp(
	const void		*a,
	const void		*b)
{
	const struct xfs_alloc_rec_incore *ap = a;
	const struct xfs_alloc_rec_incore *bp = b;

	if (ap->ar_startblock > bp->ar_startblock)
		return 1;
	else if (ap->ar_startblock < bp->ar_startblock)
		return -1;
	return 0;
}

/*
 * Compare two free space extents by length and then block number.  We want
 * to sort first in order of increasing length and then in order of increasing
 * block number.
 */
static int
xrep_cntbt_extent_cmp(
	const void			*a,
	const void			*b)
{
	const struct xfs_alloc_rec_incore *ap = a;
	const struct xfs_alloc_rec_incore *bp = b;

	if (ap->ar_blockcount > bp->ar_blockcount)
		return 1;
	else if (ap->ar_blockcount < bp->ar_blockcount)
		return -1;
	return xrep_bnobt_extent_cmp(a, b);
}

/*
 * Iterate all reverse mappings to find (1) the gaps between rmap records (all
 * unowned space), (2) the OWN_AG extents (which encompass the free space
 * btrees, the rmapbt, and the agfl), (3) the rmapbt blocks, and (4) the AGFL
 * blocks.  The free space is (1) + (2) - (3) - (4).
 */
STATIC int
xrep_abt_find_freespace(
	struct xrep_abt		*ra)
{
	struct xfs_scrub	*sc = ra->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_agf		*agf = sc->sa.agf_bp->b_addr;
	struct xfs_buf		*agfl_bp;
	xfs_agblock_t		agend;
	int			error;

	xbitmap_init(&ra->not_allocbt_blocks);

	xrep_ag_btcur_init(sc, &sc->sa);

	/*
	 * Iterate all the reverse mappings to find gaps in the physical
	 * mappings, all the OWN_AG blocks, and all the rmapbt extents.
	 */
	error = xfs_rmap_query_all(sc->sa.rmap_cur, xrep_abt_walk_rmap, ra);
	if (error)
		goto err;

	/* Insert a record for space between the last rmap and EOAG. */
	agend = be32_to_cpu(agf->agf_length);
	if (ra->next_bno < agend) {
		error = xrep_abt_stash(ra, agend);
		if (error)
			goto err;
	}

	/* Collect all the AGFL blocks. */
	error = xfs_alloc_read_agfl(mp, sc->tp, sc->sa.pag->pag_agno, &agfl_bp);
	if (error)
		goto err;

	error = xfs_agfl_walk(mp, agf, agfl_bp, xrep_abt_walk_agfl, ra);
	if (error)
		goto err_agfl;

	/* Compute the old bnobt/cntbt blocks. */
	error = xbitmap_disunion(&ra->old_allocbt_blocks,
			&ra->not_allocbt_blocks);
	if (error)
		goto err_agfl;

	ra->nr_real_records = xfarray_length(ra->free_records);
err_agfl:
	xfs_trans_brelse(sc->tp, agfl_bp);
err:
	xchk_ag_btcur_free(&sc->sa);
	xbitmap_destroy(&ra->not_allocbt_blocks);
	return error;
}

/*
 * We're going to use the observed free space records to reserve blocks for the
 * new free space btrees, so we play an iterative game where we try to converge
 * on the number of blocks we need:
 *
 * 1. Estimate how many blocks we'll need to store the records.
 * 2. If the first free record has more blocks than we need, we're done.
 *    We will have to re-sort the records prior to building the cntbt.
 * 3. If that record has exactly the number of blocks we need, null out the
 *    record.  We're done.
 * 4. Otherwise, we still need more blocks.  Null out the record, subtract its
 *    length from the number of blocks we need, and go back to step 1.
 *
 * Fortunately, we don't have to do any transaction work to play this game, so
 * we don't have to tear down the staging cursors.
 */
STATIC int
xrep_abt_reserve_space(
	struct xrep_abt		*ra,
	struct xfs_btree_cur	*bno_cur,
	struct xfs_btree_cur	*cnt_cur,
	bool			*needs_resort)
{
	struct xfs_scrub	*sc = ra->sc;
	xfarray_idx_t		record_nr;
	unsigned int		allocated = 0;
	int			error = 0;

	record_nr = xfarray_length(ra->free_records) - 1;
	do {
		struct xfs_alloc_rec_incore arec;
		xfs_fsblock_t		fsbno;
		uint64_t		required;
		unsigned int		desired;
		unsigned int		len;

		/* Compute how many blocks we'll need. */
		error = xfs_btree_bload_compute_geometry(cnt_cur,
				&ra->cnt_bload, ra->nr_real_records);
		if (error)
			break;

		error = xfs_btree_bload_compute_geometry(bno_cur,
				&ra->bno_bload, ra->nr_real_records);
		if (error)
			break;

		/* How many btree blocks do we need to store all records? */
		required = ra->cnt_bload.nr_blocks + ra->bno_bload.nr_blocks;
		ASSERT(required < INT_MAX);

		/* If we've reserved enough blocks, we're done. */
		if (allocated >= required)
			break;

		desired = required - allocated;

		/* We need space but there's none left; bye! */
		if (ra->nr_real_records == 0) {
			error = -ENOSPC;
			break;
		}

		/* Grab the first record from the list. */
		error = xfarray_load(ra->free_records, record_nr, &arec);
		if (error)
			break;

		ASSERT(arec.ar_blockcount <= UINT_MAX);
		len = min_t(unsigned int, arec.ar_blockcount, desired);
		fsbno = XFS_AGB_TO_FSB(sc->mp, sc->sa.pag->pag_agno,
				arec.ar_startblock);

		trace_xrep_newbt_alloc_blocks(sc->mp, sc->sa.pag->pag_agno,
				arec.ar_startblock, len, XFS_RMAP_OWN_AG);

		error = xrep_newbt_add_blocks(&ra->new_bnobt_info, fsbno, len);
		if (error)
			break;
		allocated += len;
		ra->nr_blocks -= len;

		if (arec.ar_blockcount > desired) {
			/*
			 * Record has more space than we need.  The number of
			 * free records doesn't change, so shrink the free
			 * record, inform the caller that the records are no
			 * longer sorted by length, and exit.
			 */
			arec.ar_startblock += desired;
			arec.ar_blockcount -= desired;
			error = xfarray_store(ra->free_records, record_nr,
					&arec);
			if (error)
				break;

			*needs_resort = true;
			return 0;
		}

		/*
		 * We're going to use up the entire record, so unset it and
		 * move on to the next one.  This changes the number of free
		 * records (but doesn't break the sorting order), so we must
		 * go around the loop once more to re-run _bload_init.
		 */
		error = xfarray_unset(ra->free_records, record_nr);
		if (error)
			break;
		ra->nr_real_records--;
		record_nr--;
	} while (1);

	return error;
}

/*
 * Deal with all the space we reserved.  Blocks that were allocated for the
 * free space btrees need to have a (deferred) rmap added for the OWN_AG
 * allocation, and blocks that didn't get used can be freed via the usual
 * (deferred) means.
 */
STATIC void
xrep_abt_dispose_reservations(
	struct xrep_abt		*ra,
	int			error)
{
	struct xrep_newbt_resv	*resv, *n;
	struct xfs_scrub	*sc = ra->sc;

	if (error)
		goto junkit;

	for_each_xrep_newbt_reservation(&ra->new_bnobt_info, resv, n) {
		/* Add a deferred rmap for each extent we used. */
		if (resv->used > 0)
			xfs_rmap_alloc_extent(sc->tp,
					XFS_FSB_TO_AGNO(sc->mp, resv->fsbno),
					XFS_FSB_TO_AGBNO(sc->mp, resv->fsbno),
					resv->used, XFS_RMAP_OWN_AG);

		/*
		 * Add a deferred free for each block we didn't use and now
		 * have to add to the free space since the new btrees are
		 * online.
		 */
		if (resv->used < resv->len)
			xfs_free_extent_later(sc->tp, resv->fsbno + resv->used,
					resv->len - resv->used, NULL,
					XFS_FREE_EXTENT_SKIP_DISCARD);
	}

junkit:
	for_each_xrep_newbt_reservation(&ra->new_bnobt_info, resv, n) {
		list_del(&resv->list);
		kfree(resv);
	}

	xrep_newbt_cancel(&ra->new_bnobt_info);
	xrep_newbt_cancel(&ra->new_cntbt_info);
}

/* Retrieve free space data for bulk load. */
STATIC int
xrep_abt_get_records(
	struct xfs_btree_cur		*cur,
	unsigned int			idx,
	struct xfs_btree_block		*block,
	unsigned int			nr_wanted,
	void				*priv)
{
	struct xfs_alloc_rec_incore	*arec = &cur->bc_rec.a;
	struct xrep_abt			*ra = priv;
	union xfs_btree_rec		*block_rec;
	unsigned int			loaded;
	int				error;

	for (loaded = 0; loaded < nr_wanted; loaded++, idx++) {
		error = xfarray_load_next(ra->free_records, &ra->array_cur,
				arec);
		if (error)
			return error;

		ra->longest = max(ra->longest, arec->ar_blockcount);

		block_rec = xfs_btree_rec_addr(cur, idx, block);
		cur->bc_ops->init_rec_from_cur(cur, block_rec);
	}

	return loaded;
}

/* Feed one of the new btree blocks to the bulk loader. */
STATIC int
xrep_abt_claim_block(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	void			*priv)
{
	struct xrep_abt		*ra = priv;

	return xrep_newbt_claim_block(cur, &ra->new_bnobt_info, ptr);
}

/*
 * Reset the AGF counters to reflect the free space btrees that we just
 * rebuilt, then reinitialize the per-AG data.
 */
STATIC int
xrep_abt_reset_counters(
	struct xrep_abt		*ra,
	unsigned int		freesp_btreeblks)
{
	struct xfs_scrub	*sc = ra->sc;
	struct xfs_perag	*pag = sc->sa.pag;
	struct xfs_agf		*agf = sc->sa.agf_bp->b_addr;
	struct xfs_buf		*bp;
	int			error;

	/*
	 * The AGF header contains extra information related to the free space
	 * btrees, so we must update those fields here.
	 */
	agf->agf_btreeblks = cpu_to_be32(freesp_btreeblks +
				(be32_to_cpu(agf->agf_rmap_blocks) - 1));
	agf->agf_freeblks = cpu_to_be32(ra->nr_blocks);
	agf->agf_longest = cpu_to_be32(ra->longest);
	xfs_alloc_log_agf(sc->tp, sc->sa.agf_bp, XFS_AGF_BTREEBLKS |
						 XFS_AGF_LONGEST |
						 XFS_AGF_FREEBLKS);

	/*
	 * After we commit the new btree to disk, it is possible that the
	 * process to reap the old btree blocks will race with the AIL trying
	 * to checkpoint the old btree blocks into the filesystem.  If the new
	 * tree is shorter than the old one, the allocbt write verifier will
	 * fail and the AIL will shut down the filesystem.
	 *
	 * To avoid this, save the old incore btree height values as the alt
	 * height values before re-initializing the perag info from the updated
	 * AGF to capture all the new values.
	 */
	pag->pagf_alt_levels[XFS_BTNUM_BNOi] = pag->pagf_levels[XFS_BTNUM_BNOi];
	pag->pagf_alt_levels[XFS_BTNUM_CNTi] = pag->pagf_levels[XFS_BTNUM_CNTi];

	/*
	 *
	 * Mark the pagf information stale and use the accessor function to
	 * forcibly reload it from the values we just logged.  We had better
	 * get the same AGF buffer.
	 */
	ASSERT(pag->pagf_init);
	pag->pagf_init = 0;
	error = xfs_alloc_read_agf(sc->mp, sc->tp, sc->sa.pag->pag_agno, 0,
			&bp);
	if (error)
		return error;

	if (bp != sc->sa.agf_bp) {
		ASSERT(bp == sc->sa.agf_bp);
		return -EFSCORRUPTED;
	}

	return 0;
}

static void
xrep_abt_init_bload(
	struct xrep_abt		*ra,
	struct xfs_btree_bload	*bload)
{
	bload->get_records = xrep_abt_get_records;
	bload->claim_block = xrep_abt_claim_block;

	xrep_bload_estimate_slack(ra->sc, bload);
}

/*
 * Use the collected free space information to stage new free space btrees.
 * If this is successful we'll return with the new btree root
 * information logged to the repair transaction but not yet committed.
 */
STATIC int
xrep_abt_build_new_trees(
	struct xrep_abt		*ra)
{
	struct xfs_scrub	*sc = ra->sc;
	struct xfs_btree_cur	*bno_cur;
	struct xfs_btree_cur	*cnt_cur;
	struct xfs_perag	*pag = sc->sa.pag;
	bool			needs_resort;
	int			error;

	xrep_abt_init_bload(ra, &ra->bno_bload);
	xrep_abt_init_bload(ra, &ra->cnt_bload);

	/*
	 * Sort the free extents by length so that we can set up the free space
	 * btrees in as few extents as possible.  This reduces the amount of
	 * deferred rmap / free work we have to do at the end.
	 */
	error = xfarray_sort(ra->free_records, xrep_cntbt_extent_cmp,
			XFARRAY_SORT_KILLABLE);
	if (error)
		return error;
	needs_resort = false;

	/*
	 * Prepare to construct the new btree by reserving disk space for the
	 * new btree and setting up all the accounting information we'll need
	 * to root the new btree while it's under construction and before we
	 * attach it to the AG header.
	 */
	xrep_newbt_init_bare(&ra->new_bnobt_info, sc);
	xrep_newbt_init_bare(&ra->new_cntbt_info, sc);

	/* Allocate cursors for the staged btrees. */
	bno_cur = xfs_allocbt_stage_cursor(sc->mp, &ra->new_bnobt_info.afake,
			pag, XFS_BTNUM_BNO);
	cnt_cur = xfs_allocbt_stage_cursor(sc->mp, &ra->new_cntbt_info.afake,
			pag, XFS_BTNUM_CNT);

	/* Last chance to abort before we start committing fixes. */
	if (xchk_should_terminate(sc, &error))
		goto err_cur;

	/* Reserve the space we'll need for the new btrees. */
	error = xrep_abt_reserve_space(ra, bno_cur, cnt_cur, &needs_resort);
	if (error)
		goto err_cur;

	/*
	 * If we need to re-sort the free extents by length, do so so that we
	 * can put the records into the cntbt in the correct order.
	 */
	if (needs_resort) {
		error = xfarray_sort(ra->free_records, xrep_cntbt_extent_cmp,
				0);
		if (error)
			goto err_cur;
	}

	/*
	 * Due to btree slack factors, it's possible for a new btree to be one
	 * level taller than the old btree.  Update the alternate incore btree
	 * height so that we don't trip the verifiers when writing the new
	 * btree blocks to disk.
	 */
	pag->pagf_alt_levels[XFS_BTNUM_BNOi] = ra->bno_bload.btree_height;
	pag->pagf_alt_levels[XFS_BTNUM_CNTi] = ra->cnt_bload.btree_height;

	/* Load the free space by length tree. */
	ra->array_cur = XFARRAY_CURSOR_INIT;
	ra->longest = 0;
	error = xfs_btree_bload(cnt_cur, &ra->cnt_bload, ra);
	if (error)
		goto err_levels;

	/* Re-sort the free extents by block number so so that we can put the
	 * records into the bnobt in the correct order.
	 */
	error = xfarray_sort(ra->free_records, xrep_bnobt_extent_cmp, 0);
	if (error)
		goto err_levels;

	/* Load the free space by block number tree. */
	ra->array_cur = XFARRAY_CURSOR_INIT;
	error = xfs_btree_bload(bno_cur, &ra->bno_bload, ra);
	if (error)
		goto err_levels;

	/*
	 * Install the new btrees in the AG header.  After this point the old
	 * btrees are no longer accessible and the new trees are live.
	 */
	xfs_allocbt_commit_staged_btree(bno_cur, sc->tp, sc->sa.agf_bp);
	xfs_btree_del_cursor(bno_cur, 0);
	xfs_allocbt_commit_staged_btree(cnt_cur, sc->tp, sc->sa.agf_bp);
	xfs_btree_del_cursor(cnt_cur, 0);

	/* Reset the AGF counters now that we've changed the btree shape. */
	error = xrep_abt_reset_counters(ra, (ra->bno_bload.nr_blocks - 1) +
					    (ra->cnt_bload.nr_blocks - 1));
	if (error)
		goto err_newbt;

	/* Dispose of any unused blocks and the accounting information. */
	xrep_abt_dispose_reservations(ra, error);

	return xrep_roll_ag_trans(sc);

err_levels:
	pag->pagf_alt_levels[XFS_BTNUM_BNOi] = 0;
	pag->pagf_alt_levels[XFS_BTNUM_CNTi] = 0;
err_cur:
	xfs_btree_del_cursor(cnt_cur, error);
	xfs_btree_del_cursor(bno_cur, error);
err_newbt:
	xrep_abt_dispose_reservations(ra, error);
	return error;
}

/*
 * Now that we've logged the roots of the new btrees, invalidate all of the
 * old blocks and free them.
 */
STATIC int
xrep_abt_remove_old_trees(
	struct xrep_abt		*ra)
{
	struct xfs_perag	*pag = ra->sc->sa.pag;
	int			error;

	/* Free the old inode btree blocks if they're not in use. */
	error = xrep_reap_extents(ra->sc, &ra->old_allocbt_blocks,
			&XFS_RMAP_OINFO_AG, XFS_AG_RESV_IGNORE);
	if (error)
		return error;

	/*
	 * Now that we've zapped all the old allocbt blocks we can turn off
	 * the alternate height mechanism.
	 */
	pag->pagf_alt_levels[XFS_BTNUM_BNOi] = 0;
	pag->pagf_alt_levels[XFS_BTNUM_CNTi] = 0;
	return 0;
}

/* Repair the freespace btrees for some AG. */
int
xrep_allocbt(
	struct xfs_scrub	*sc)
{
	struct xrep_abt		*ra;
	struct xfs_mount	*mp = sc->mp;
	int			error;

	/* We require the rmapbt to rebuild anything. */
	if (!xfs_has_rmapbt(mp))
		return -EOPNOTSUPP;

	ra = kzalloc(sizeof(struct xrep_abt), XCHK_GFP_FLAGS);
	if (!ra)
		return -ENOMEM;
	ra->sc = sc;

	/* We rebuild both data structures. */
	sc->sick_mask = XFS_SICK_AG_BNOBT | XFS_SICK_AG_CNTBT;

	/*
	 * Make sure the busy extent list is clear because we can't put extents
	 * on there twice.  In theory we cleared this before we started, but
	 * let's not risk the filesystem.
	 */
	if (!xfs_extent_busy_list_empty(sc->sa.pag)) {
		error = -EDEADLOCK;
		goto out_ra;
	}

	/* Set up enough storage to handle maximally fragmented free space. */
	error = xfarray_create(mp, "free space extents",
			mp->m_sb.sb_agblocks / 2,
			sizeof(struct xfs_alloc_rec_incore),
			&ra->free_records);
	if (error)
		goto out_ra;

	/* Collect the free space data and find the old btree blocks. */
	xbitmap_init(&ra->old_allocbt_blocks);
	error = xrep_abt_find_freespace(ra);
	if (error)
		goto out_bitmap;

	/* Rebuild the free space information. */
	error = xrep_abt_build_new_trees(ra);
	if (error)
		goto out_bitmap;

	/* Kill the old trees. */
	error = xrep_abt_remove_old_trees(ra);

out_bitmap:
	xbitmap_destroy(&ra->old_allocbt_blocks);
	xfarray_destroy(ra->free_records);
out_ra:
	kfree(ra);
	return error;
}

/* Make sure both btrees are ok after we've rebuilt them. */
int
xrep_revalidate_allocbt(
	struct xfs_scrub	*sc)
{
	__u32			old_type = sc->sm->sm_type;
	int			error;

	/*
	 * We must update sm_type temporarily so that the tree-to-tree cross
	 * reference checks will work in the correct direction, and also so
	 * that tracing will report correctly if there are more errors.
	 */
	sc->sm->sm_type = XFS_SCRUB_TYPE_BNOBT;
	error = xchk_bnobt(sc);
	if (error)
		goto out;

	sc->sm->sm_type = XFS_SCRUB_TYPE_CNTBT;
	error = xchk_cntbt(sc);
out:
	sc->sm->sm_type = old_type;
	return error;
}
