// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_btree.h"
#include "xfs_btree_staging.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_log.h"
#include "xfs_sb.h"
#include "xfs_inode.h"
#include "xfs_alloc.h"
#include "xfs_alloc_btree.h"
#include "xfs_ialloc.h"
#include "xfs_ialloc_btree.h"
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_refcount_btree.h"
#include "xfs_extent_busy.h"
#include "xfs_ag.h"
#include "xfs_ag_resv.h"
#include "xfs_quota.h"
#include "xfs_qm.h"
#include "xfs_bmap.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_attr.h"
#include "xfs_attr_remote.h"
#include "xfs_defer.h"
#include "xfs_extfree_item.h"
#include "xfs_errortag.h"
#include "xfs_error.h"
#include "xfs_reflink.h"
#include "xfs_health.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/bitmap.h"

/*
 * Attempt to repair some metadata, if the metadata is corrupt and userspace
 * told us to fix it.  This function returns -EAGAIN to mean "re-run scrub",
 * and will set *fixed to true if it thinks it repaired anything.
 */
int
xrep_attempt(
	struct xfs_scrub	*sc)
{
	int			error = 0;

	trace_xrep_attempt(XFS_I(file_inode(sc->file)), sc->sm, error);

	xchk_ag_btcur_free(&sc->sa);

	/* Repair whatever's broken. */
	ASSERT(sc->ops->repair);
	error = sc->ops->repair(sc);
	trace_xrep_done(XFS_I(file_inode(sc->file)), sc->sm, error);
	switch (error) {
	case 0:
		/*
		 * Repair succeeded.  Commit the fixes and perform a second
		 * scrub so that we can tell userspace if we fixed the problem.
		 */
		sc->sm->sm_flags &= ~XFS_SCRUB_FLAGS_OUT;
		sc->flags |= XREP_ALREADY_FIXED;
		return -EAGAIN;
	case -EDEADLOCK:
	case -EAGAIN:
		/* Tell the caller to try again having grabbed all the locks. */
		if (!(sc->flags & XCHK_TRY_HARDER)) {
			sc->flags |= XCHK_TRY_HARDER;
			return -EAGAIN;
		}
		/*
		 * We tried harder but still couldn't grab all the resources
		 * we needed to fix it.  The corruption has not been fixed,
		 * so exit to userspace.
		 */
		return 0;
	default:
		return error;
	}
}

/*
 * Complain about unfixable problems in the filesystem.  We don't log
 * corruptions when IFLAG_REPAIR wasn't set on the assumption that the driver
 * program is xfs_scrub, which will call back with IFLAG_REPAIR set if the
 * administrator isn't running xfs_scrub in no-repairs mode.
 *
 * Use this helper function because _ratelimited silently declares a static
 * structure to track rate limiting information.
 */
void
xrep_failure(
	struct xfs_mount	*mp)
{
	xfs_alert_ratelimited(mp,
"Corruption not fixed during online repair.  Unmount and run xfs_repair.");
}

/*
 * Repair probe -- userspace uses this to probe if we're willing to repair a
 * given mountpoint.
 */
int
xrep_probe(
	struct xfs_scrub	*sc)
{
	int			error = 0;

	if (xchk_should_terminate(sc, &error))
		return error;

	return 0;
}

/*
 * Roll a transaction, keeping the AG headers locked and reinitializing
 * the btree cursors.
 */
int
xrep_roll_ag_trans(
	struct xfs_scrub	*sc)
{
	int			error;

	/* Keep the AG header buffers locked so we can keep going. */
	if (sc->sa.agi_bp)
		xfs_trans_bhold(sc->tp, sc->sa.agi_bp);
	if (sc->sa.agf_bp)
		xfs_trans_bhold(sc->tp, sc->sa.agf_bp);

	/*
	 * Roll the transaction.  We still own the buffer and the buffer lock
	 * regardless of whether or not the roll succeeds.  If the roll fails,
	 * the buffers will be released during teardown on our way out of the
	 * kernel.  If it succeeds, we join them to the new transaction and
	 * move on.
	 */
	error = xfs_trans_roll(&sc->tp);
	if (error)
		return error;

	/*
	 * Join AG headers to the new transaction.  The buffer log item can
	 * detach from the buffer across the transaction roll if the bli is
	 * clean, so ensure the buffer type is still set on the AG header
	 * buffers' blis before we return.
	 *
	 * Normal code would never hold a clean buffer across a roll, but scrub
	 * needs both buffers to maintain a total lock on the AG.
	 */
	if (sc->sa.agi_bp) {
		xfs_trans_bjoin(sc->tp, sc->sa.agi_bp);
		xfs_trans_buf_set_type(sc->tp, sc->sa.agi_bp, XFS_BLFT_AGI_BUF);
	}
	if (sc->sa.agf_bp) {
		xfs_trans_bjoin(sc->tp, sc->sa.agf_bp);
		xfs_trans_buf_set_type(sc->tp, sc->sa.agf_bp, XFS_BLFT_AGF_BUF);
	}

	return 0;
}

/* Roll the scrub transaction, holding the primary metadata locked. */
int
xrep_roll_trans(
	struct xfs_scrub	*sc)
{
	if (!sc->ip)
		return xrep_roll_ag_trans(sc);
	return xfs_trans_roll_inode(&sc->tp, sc->ip);
}

/* Finish all deferred work attached to the repair transaction. */
STATIC int
xrep_defer_finish(
	struct xfs_scrub	*sc)
{
	int			error;

	/* Keep the AG header buffers locked so we can keep going. */
	if (sc->sa.agi_bp) {
		xfs_ialloc_log_agi(sc->tp, sc->sa.agi_bp, XFS_AGI_MAGICNUM);
		xfs_trans_bhold(sc->tp, sc->sa.agi_bp);
	}

	if (sc->sa.agf_bp) {
		xfs_alloc_log_agf(sc->tp, sc->sa.agf_bp, XFS_AGF_MAGICNUM);
		xfs_trans_bhold(sc->tp, sc->sa.agf_bp);
	}

	error = xfs_defer_finish(&sc->tp);
	if (error)
		return error;

	/*
	 * The buffer log item (and hence the blf type) can detach from
	 * the buffer across the transaction rolls, so ensure that the
	 * types are still set on the AG header buffers.  Release the hold
	 * that we set above because defer_finish won't do that for us.
	 */
	if (sc->sa.agi_bp) {
		xfs_trans_bhold_release(sc->tp, sc->sa.agi_bp);
		xfs_trans_buf_set_type(sc->tp, sc->sa.agi_bp, XFS_BLFT_AGI_BUF);
	}
	if (sc->sa.agf_bp) {
		xfs_trans_bhold_release(sc->tp, sc->sa.agf_bp);
		xfs_trans_buf_set_type(sc->tp, sc->sa.agf_bp, XFS_BLFT_AGF_BUF);
	}
	return 0;
}

/*
 * Does the given AG have enough space to rebuild a btree?  Neither AG
 * reservation can be critical, and we must have enough space (factoring
 * in AG reservations) to construct a whole btree.
 */
bool
xrep_ag_has_space(
	struct xfs_perag	*pag,
	xfs_extlen_t		nr_blocks,
	enum xfs_ag_resv_type	type)
{
	return  !xfs_ag_resv_critical(pag, XFS_AG_RESV_RMAPBT) &&
		!xfs_ag_resv_critical(pag, XFS_AG_RESV_METADATA) &&
		pag->pagf_freeblks > xfs_ag_resv_needed(pag, type) + nr_blocks;
}

/*
 * Figure out how many blocks to reserve for an AG repair.  We calculate the
 * worst case estimate for the number of blocks we'd need to rebuild one of
 * any type of per-AG btree.
 */
xfs_extlen_t
xrep_calc_ag_resblks(
	struct xfs_scrub		*sc)
{
	struct xfs_mount		*mp = sc->mp;
	struct xfs_scrub_metadata	*sm = sc->sm;
	struct xfs_perag		*pag;
	struct xfs_buf			*bp;
	xfs_agino_t			icount = NULLAGINO;
	xfs_extlen_t			aglen = NULLAGBLOCK;
	xfs_extlen_t			usedlen;
	xfs_extlen_t			freelen;
	xfs_extlen_t			bnobt_sz;
	xfs_extlen_t			inobt_sz;
	xfs_extlen_t			rmapbt_sz;
	xfs_extlen_t			refcbt_sz;
	int				error;

	if (!(sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR))
		return 0;

	pag = xfs_perag_get(mp, sm->sm_agno);
	if (pag->pagi_init) {
		/* Use in-core icount if possible. */
		icount = pag->pagi_count;
	} else {
		/* Try to get the actual counters from disk. */
		error = xfs_ialloc_read_agi(mp, NULL, sm->sm_agno, &bp);
		if (!error) {
			icount = pag->pagi_count;
			xfs_buf_relse(bp);
		}
	}

	/* Now grab the block counters from the AGF. */
	error = xfs_alloc_read_agf(mp, NULL, sm->sm_agno, 0, &bp);
	if (error) {
		aglen = xfs_ag_block_count(mp, sm->sm_agno);
		freelen = aglen;
		usedlen = aglen;
	} else {
		struct xfs_agf	*agf = bp->b_addr;

		aglen = be32_to_cpu(agf->agf_length);
		freelen = be32_to_cpu(agf->agf_freeblks);
		usedlen = aglen - freelen;
		xfs_buf_relse(bp);
	}
	xfs_perag_put(pag);

	/* If the icount is impossible, make some worst-case assumptions. */
	if (icount == NULLAGINO ||
	    !xfs_verify_agino(mp, sm->sm_agno, icount)) {
		xfs_agino_t	first, last;

		xfs_agino_range(mp, sm->sm_agno, &first, &last);
		icount = last - first + 1;
	}

	/* If the block counts are impossible, make worst-case assumptions. */
	if (aglen == NULLAGBLOCK ||
	    aglen != xfs_ag_block_count(mp, sm->sm_agno) ||
	    freelen >= aglen) {
		aglen = xfs_ag_block_count(mp, sm->sm_agno);
		freelen = aglen;
		usedlen = aglen;
	}

	trace_xrep_calc_ag_resblks(mp, sm->sm_agno, icount, aglen,
			freelen, usedlen);

	/*
	 * Figure out how many blocks we'd need worst case to rebuild
	 * each type of btree.  Note that we can only rebuild the
	 * bnobt/cntbt or inobt/finobt as pairs.
	 */
	bnobt_sz = 2 * xfs_allocbt_calc_size(mp, freelen);
	if (xfs_has_sparseinodes(mp))
		inobt_sz = xfs_iallocbt_calc_size(mp, icount /
				XFS_INODES_PER_HOLEMASK_BIT);
	else
		inobt_sz = xfs_iallocbt_calc_size(mp, icount /
				XFS_INODES_PER_CHUNK);
	if (xfs_has_finobt(mp))
		inobt_sz *= 2;
	if (xfs_has_reflink(mp))
		refcbt_sz = xfs_refcountbt_calc_size(mp, usedlen);
	else
		refcbt_sz = 0;
	if (xfs_has_rmapbt(mp)) {
		/*
		 * Guess how many blocks we need to rebuild the rmapbt.
		 * For non-reflink filesystems we can't have more records than
		 * used blocks.  However, with reflink it's possible to have
		 * more than one rmap record per AG block.  We don't know how
		 * many rmaps there could be in the AG, so we start off with
		 * what we hope is an generous over-estimation.
		 */
		if (xfs_has_reflink(mp))
			rmapbt_sz = xfs_rmapbt_calc_size(mp,
					(unsigned long long)aglen * 2);
		else
			rmapbt_sz = xfs_rmapbt_calc_size(mp, usedlen);
	} else {
		rmapbt_sz = 0;
	}

	trace_xrep_calc_ag_resblks_btsize(mp, sm->sm_agno, bnobt_sz,
			inobt_sz, rmapbt_sz, refcbt_sz);

	return max(max(bnobt_sz, inobt_sz), max(rmapbt_sz, refcbt_sz));
}

/* Allocate a block in an AG. */
int
xrep_alloc_ag_block(
	struct xfs_scrub		*sc,
	const struct xfs_owner_info	*oinfo,
	xfs_fsblock_t			*fsbno,
	enum xfs_ag_resv_type		resv)
{
	struct xfs_alloc_arg		args = {0};
	xfs_agblock_t			bno;
	int				error;

	switch (resv) {
	case XFS_AG_RESV_AGFL:
	case XFS_AG_RESV_RMAPBT:
		error = xfs_alloc_get_freelist(sc->tp, sc->sa.agf_bp, &bno, 1);
		if (error)
			return error;
		if (bno == NULLAGBLOCK)
			return -ENOSPC;
		xfs_extent_busy_reuse(sc->mp, sc->sa.pag, bno,
				1, false);
		*fsbno = XFS_AGB_TO_FSB(sc->mp, sc->sa.pag->pag_agno, bno);
		if (resv == XFS_AG_RESV_RMAPBT)
			xfs_ag_resv_rmapbt_alloc(sc->mp, sc->sa.pag->pag_agno);
		return 0;
	default:
		break;
	}

	args.tp = sc->tp;
	args.mp = sc->mp;
	args.oinfo = *oinfo;
	args.fsbno = XFS_AGB_TO_FSB(args.mp, sc->sa.pag->pag_agno, 0);
	args.minlen = 1;
	args.maxlen = 1;
	args.prod = 1;
	args.type = XFS_ALLOCTYPE_THIS_AG;
	args.resv = resv;

	error = xfs_alloc_vextent(&args);
	if (error)
		return error;
	if (args.fsbno == NULLFSBLOCK)
		return -ENOSPC;
	ASSERT(args.len == 1);
	*fsbno = args.fsbno;

	return 0;
}

/* Initialize a new AG btree root block with zero entries. */
int
xrep_init_btblock(
	struct xfs_scrub		*sc,
	xfs_fsblock_t			fsb,
	struct xfs_buf			**bpp,
	xfs_btnum_t			btnum,
	const struct xfs_buf_ops	*ops)
{
	struct xfs_trans		*tp = sc->tp;
	struct xfs_mount		*mp = sc->mp;
	struct xfs_buf			*bp;
	int				error;

	trace_xrep_init_btblock(mp, XFS_FSB_TO_AGNO(mp, fsb),
			XFS_FSB_TO_AGBNO(mp, fsb), btnum);

	ASSERT(XFS_FSB_TO_AGNO(mp, fsb) == sc->sa.pag->pag_agno);
	error = xfs_trans_get_buf(tp, mp->m_ddev_targp,
			XFS_FSB_TO_DADDR(mp, fsb), XFS_FSB_TO_BB(mp, 1), 0,
			&bp);
	if (error)
		return error;
	xfs_buf_zero(bp, 0, BBTOB(bp->b_length));
	xfs_btree_init_block(mp, bp, btnum, 0, 0, sc->sa.pag->pag_agno);
	xfs_trans_buf_set_type(tp, bp, XFS_BLFT_BTREE_BUF);
	xfs_trans_log_buf(tp, bp, 0, BBTOB(bp->b_length) - 1);
	bp->b_ops = ops;
	*bpp = bp;

	return 0;
}

/* Initialize accounting resources for staging a new AG btree. */
void
xrep_newbt_init_ag(
	struct xrep_newbt		*xnr,
	struct xfs_scrub		*sc,
	const struct xfs_owner_info	*oinfo,
	xfs_fsblock_t			alloc_hint,
	enum xfs_ag_resv_type		resv)
{
	memset(xnr, 0, sizeof(struct xrep_newbt));
	xnr->sc = sc;
	xnr->oinfo = *oinfo; /* structure copy */
	xnr->alloc_hint = alloc_hint;
	xnr->resv = resv;
	INIT_LIST_HEAD(&xnr->resv_list);
}

/* Initialize accounting resources for staging a new inode fork btree. */
int
xrep_newbt_init_inode(
	struct xrep_newbt		*xnr,
	struct xfs_scrub		*sc,
	int				whichfork,
	const struct xfs_owner_info	*oinfo)
{
	struct xfs_ifork		*ifp;

	ifp = kmem_cache_zalloc(xfs_ifork_cache, XCHK_GFP_FLAGS);
	if (!ifp)
		return -ENOMEM;

	xrep_newbt_init_ag(xnr, sc, oinfo,
			XFS_INO_TO_FSB(sc->mp, sc->ip->i_ino),
			XFS_AG_RESV_NONE);
	xnr->ifake.if_fork = ifp;
	xnr->ifake.if_fork_size = XFS_IFORK_SIZE(sc->ip, whichfork);
	xnr->ifake.if_whichfork = whichfork;
	return 0;
}

/*
 * Initialize accounting resources for staging a new btree.  Callers are
 * expected to add their own reservations (and clean them up) manually.
 */
void
xrep_newbt_init_bare(
	struct xrep_newbt		*xnr,
	struct xfs_scrub		*sc)
{
	xrep_newbt_init_ag(xnr, sc, &XFS_RMAP_OINFO_ANY_OWNER, NULLFSBLOCK,
			XFS_AG_RESV_NONE);
}

/*
 * Set up automatic reaping of the blocks reserved for btree reconstruction in
 * case we crash by logging a deferred free item for each extent we allocate so
 * that we can get all of the space back if we crash before we can commit the
 * new btree.  This function returns a token that can be used to cancel
 * automatic reaping if repair is successful.
 */
static int
xrep_newbt_schedule_autoreap(
	struct xrep_newbt		*xnr,
	struct xrep_newbt_resv		*resv)
{
	struct xfs_extent_free_item	efi_item = {
		.xefi_startblock	= resv->fsbno,
		.xefi_blockcount	= resv->len,
		.xefi_owner		= xnr->oinfo.oi_owner,
		.xefi_flags		= XFS_EFI_SKIP_DISCARD,
	};
	struct xfs_log_item		*lip;
	LIST_HEAD(items);

	ASSERT(xnr->oinfo.oi_offset == 0);

	if (xnr->oinfo.oi_flags & XFS_OWNER_INFO_ATTR_FORK)
		efi_item.xefi_flags |= XFS_EFI_ATTR_FORK;
	if (xnr->oinfo.oi_flags & XFS_OWNER_INFO_BMBT_BLOCK)
		efi_item.xefi_flags |= XFS_EFI_BMBT_BLOCK;

	INIT_LIST_HEAD(&efi_item.xefi_list);
	list_add(&efi_item.xefi_list, &items);
	xfs_fs_bump_intents(xnr->sc->mp, resv->fsbno);
	lip = xfs_extent_free_defer_type.create_intent(xnr->sc->tp,
			&items, 1, false);
	if (!lip) {
		ASSERT(0);
		xfs_fs_drop_intents(xnr->sc->mp, resv->fsbno);
		return -EFSCORRUPTED;
	}
	if (IS_ERR(lip)) {
		xfs_fs_drop_intents(xnr->sc->mp, resv->fsbno);
		return PTR_ERR(lip);
	}

	resv->efi = lip;
	return 0;
}

/*
 * Earlier, we logged EFIs for the extents that we allocated to hold the new
 * btree so that we could automatically roll back those allocations if the
 * system crashed.  Now we log an EFD to cancel the EFI, either because the
 * repair succeeded and the new blocks are in use; or because the repair was
 * cancelled and we're about to free the extents directly.
 */
static inline void
xrep_newbt_finish_autoreap(
	struct xfs_scrub	*sc,
	struct xrep_newbt_resv	*resv)
{
	struct xfs_efd_log_item	*efdp;
	struct xfs_extent	*extp;
	struct xfs_log_item	*efd_lip;

	efd_lip = xfs_extent_free_defer_type.create_done(sc->tp, resv->efi, 1);
	efdp = container_of(efd_lip, struct xfs_efd_log_item, efd_item);
	extp = efdp->efd_format.efd_extents;
	extp->ext_start = resv->fsbno;
	extp->ext_len = resv->len;
	efdp->efd_next_extent++;
	set_bit(XFS_LI_DIRTY, &efd_lip->li_flags);
}

/* Abort an EFI logged for a new btree block reservation. */
static inline void
xrep_newbt_cancel_autoreap(
	struct xrep_newbt_resv	*resv)
{
	xfs_extent_free_defer_type.abort_intent(resv->efi);
}

/*
 * Relog the EFIs attached to a staging btree so that we don't pin the log
 * tail.  Same logic as xfs_defer_relog.
 */
int
xrep_newbt_relog_autoreap(
	struct xrep_newbt	*xnr)
{
	struct xrep_newbt_resv	*resv;
	unsigned int		efi_bytes = 0;

	list_for_each_entry(resv, &xnr->resv_list, list) {
		/*
		 * If the log intent item for this deferred op is in a
		 * different checkpoint, relog it to keep the log tail moving
		 * forward.  We're ok with this being racy because an incorrect
		 * decision means we'll be a little slower at pushing the tail.
		 */
		if (!resv->efi || xfs_log_item_in_current_chkpt(resv->efi))
			continue;

		resv->efi = xfs_trans_item_relog(resv->efi, xnr->sc->tp);

		/*
		 * If free space is very fragmented, it's possible that the new
		 * btree will be allocated a large number of small extents.
		 * On an active system, it's possible that so many of those
		 * EFIs will need relogging here that doing them all in one
		 * transaction will overflow the reservation.
		 *
		 * Each allocation for the new btree (xrep_newbt_resv) points
		 * to a unique single-mapping EFI, so each relog operation logs
		 * a single-mapping EFD followed by a new EFI.  Each single
		 * mapping EF[ID] item consumes about 128 bytes, so we'll
		 * assume 256 bytes per relog.  Roll if we consume more than
		 * half of the transaction reservation.
		 */
		efi_bytes += 256;
		if (efi_bytes > xnr->sc->tp->t_log_res / 2) {
			int	error;

			error = xrep_roll_trans(xnr->sc);
			if (error)
				return error;

			efi_bytes = 0;
		}
	}

	if (xnr->sc->tp->t_flags & XFS_TRANS_DIRTY)
		return xrep_roll_trans(xnr->sc);
	return 0;
}

/* Designate specific blocks to be used to build our new btree. */
static int
__xrep_newbt_add_blocks(
	struct xrep_newbt		*xnr,
	xfs_fsblock_t			fsbno,
	xfs_extlen_t			len,
	bool				auto_reap)
{
	struct xrep_newbt_resv		*resv;
	int				error;

	resv = kmalloc(sizeof(struct xrep_newbt_resv), XCHK_GFP_FLAGS);
	if (!resv)
		return -ENOMEM;

	INIT_LIST_HEAD(&resv->list);
	resv->fsbno = fsbno;
	resv->len = len;
	resv->used = 0;
	if (auto_reap) {
		error = xrep_newbt_schedule_autoreap(xnr, resv);
		if (error) {
			kfree(resv);
			return error;
		}
	}

	list_add_tail(&resv->list, &xnr->resv_list);
	return 0;
}

/*
 * Allow certain callers to add disk space directly to the reservation.
 * Callers are responsible for cleaning up the reservations.
 */
int
xrep_newbt_add_blocks(
	struct xrep_newbt		*xnr,
	xfs_fsblock_t			fsbno,
	xfs_extlen_t			len)
{
	return __xrep_newbt_add_blocks(xnr, fsbno, len, false);
}

/* Allocate disk space for our new btree. */
int
xrep_newbt_alloc_blocks(
	struct xrep_newbt	*xnr,
	uint64_t		nr_blocks)
{
	struct xfs_scrub	*sc = xnr->sc;
	xfs_alloctype_t		type;
	xfs_fsblock_t		alloc_hint = xnr->alloc_hint;
	int			error = 0;

	/*
	 * Inode-rooted btrees can allocate from any AG, whereas AG btrees
	 * require a specific AG mentioned in the alloc hint..
	 */
	type = sc->ip ? XFS_ALLOCTYPE_START_BNO : XFS_ALLOCTYPE_NEAR_BNO;

	while (nr_blocks > 0) {
		struct xfs_alloc_arg	args = {
			.tp		= sc->tp,
			.mp		= sc->mp,
			.type		= type,
			.fsbno		= alloc_hint,
			.oinfo		= xnr->oinfo,
			.minlen		= 1,
			.maxlen		= nr_blocks,
			.prod		= 1,
			.resv		= xnr->resv,
		};

		error = xfs_alloc_vextent(&args);
		if (error)
			return error;
		if (args.fsbno == NULLFSBLOCK)
			return -ENOSPC;

		trace_xrep_newbt_alloc_blocks(sc->mp,
				XFS_FSB_TO_AGNO(sc->mp, args.fsbno),
				XFS_FSB_TO_AGBNO(sc->mp, args.fsbno),
				args.len, xnr->oinfo.oi_owner);

		error = __xrep_newbt_add_blocks(xnr, args.fsbno, args.len,
				true);
		if (error)
			return error;

		nr_blocks -= args.len;
		alloc_hint = args.fsbno + args.len - 1;

		error = xrep_defer_finish(sc);
		if (error)
			return error;
	}

	return 0;
}

/* Free the in-memory parts of the reservation. */
static inline void
xrep_newbt_free_resv(
	struct xrep_newbt	*xnr)
{
	struct xfs_scrub	*sc = xnr->sc;
	struct xrep_newbt_resv	*resv, *n;

	/*
	 * If we still have reservations attached to @newbt, cleanup must have
	 * failed and the filesystem is about to go down.  Clean up the incore
	 * reservations.
	 */
	list_for_each_entry_safe(resv, n, &xnr->resv_list, list) {
		xrep_newbt_cancel_autoreap(resv);
		xfs_fs_drop_intents(sc->mp, resv->fsbno);
		list_del(&resv->list);
		kfree(resv);
	}

	if (sc->ip) {
		kmem_cache_free(xfs_ifork_cache, xnr->ifake.if_fork);
		xnr->ifake.if_fork = NULL;
	}
}

/*
 * Release blocks that were reserved for a btree repair.  If the repair
 * succeeded then we log deferred frees for unused blocks.  Otherwise, we try
 * to free the extents immediately to roll the filesystem back to where it was
 * before we started.
 */
static inline void
xrep_newbt_cancel_resv(
	struct xrep_newbt	*xnr,
	struct xrep_newbt_resv	*resv)
{
	struct xfs_scrub	*sc = xnr->sc;

	xrep_newbt_finish_autoreap(sc, resv);

	trace_xrep_newbt_cancel_blocks(sc->mp,
			XFS_FSB_TO_AGNO(sc->mp, resv->fsbno),
			XFS_FSB_TO_AGBNO(sc->mp, resv->fsbno),
			resv->len, xnr->oinfo.oi_owner);

	__xfs_free_extent_later(sc->tp, resv->fsbno, resv->len,
			&xnr->oinfo, true);

	/* Drop the intent drain after we commit the new item. */
	xfs_fs_drop_intents(sc->mp, resv->fsbno);
}

/*
 * How many extent freeing items can we attach to a transaction before we want
 * to finish the chain so that unreserving new btree blocks doesn't overrun
 * the transaction reservation?
 */
#define XREP_REAP_MAX_NEWBT_EFIS	(128)

/*
 * Free all the accounting info and disk space we reserved for a new btree.
 * We want to try to roll things back cleanly for things like ENOSPC.
 */
void
xrep_newbt_cancel(
	struct xrep_newbt	*xnr)
{
	struct xfs_scrub	*sc = xnr->sc;
	struct xrep_newbt_resv	*resv, *n;
	unsigned int		freed = 0;
	int			error = 0;

	/*
	 * If the filesystem already went down, we can't free the blocks.  Skip
	 * ahead to freeing the incore metadata because we can't fix anything.
	 */
	if (xfs_is_shutdown(sc->mp))
		goto junkit;

	list_for_each_entry_safe(resv, n, &xnr->resv_list, list) {
		xrep_newbt_cancel_resv(xnr, resv);
		list_del(&resv->list);
		kfree(resv);

		if (++freed >= XREP_REAP_MAX_NEWBT_EFIS) {
			error = xrep_defer_finish(sc);
			if (error)
				goto junkit;
			freed = 0;
		}
	}

	/*
	 * If we made it all the way here without errors, roll the transaction
	 * to commit the rollbacks cleanly.
	 */
	if (freed)
		xrep_defer_finish(sc);
junkit:
	xrep_newbt_free_resv(xnr);
}

/*
 * Release blocks that were reserved for a btree repair.  If the repair
 * succeeded then we log deferred frees for unused blocks.  Otherwise, we try
 * to free the extents immediately to roll the filesystem back to where it was
 * before we started.
 */
static inline void
xrep_newbt_destroy_resv(
	struct xrep_newbt	*xnr,
	struct xrep_newbt_resv	*resv)
{
	struct xfs_scrub	*sc = xnr->sc;
	xfs_fsblock_t		fsbno = resv->fsbno;

	xrep_newbt_finish_autoreap(sc, resv);

	/*
	 * Use the deferred freeing mechanism to schedule for deletion any
	 * blocks we didn't use to rebuild the tree.  This enables us to log
	 * them all in the same transaction as the root change.
	 */
	resv->fsbno += resv->used;
	resv->len -= resv->used;
	resv->used = 0;

	/*
	 * Note: It is not safe to use resv->fsbno if we return with resv->len
	 * because it could point past the end of the original reservation!
	 */
	if (resv->len > 0) {
		trace_xrep_newbt_free_blocks(sc->mp,
				XFS_FSB_TO_AGNO(sc->mp, resv->fsbno),
				XFS_FSB_TO_AGBNO(sc->mp, resv->fsbno),
				resv->len, xnr->oinfo.oi_owner);

		__xfs_free_extent_later(sc->tp, resv->fsbno, resv->len,
				&xnr->oinfo, true);
	}

	/*
	 * Drop the intent drain after we commit the new item.  Use the
	 * original fsbno from the reservation because destroying the
	 * reservation consumes resv->fsbno.
	 */
	xfs_fs_drop_intents(sc->mp, fsbno);
}

/* Free all the accounting info and disk space we reserved for a new btree. */
int
xrep_newbt_destroy(
	struct xrep_newbt	*xnr)
{
	struct xfs_scrub	*sc = xnr->sc;
	struct xrep_newbt_resv	*resv, *n;
	unsigned int		freed = 0;
	int			error = 0;

	list_for_each_entry_safe(resv, n, &xnr->resv_list, list) {
		xrep_newbt_destroy_resv(xnr, resv);
		list_del(&resv->list);
		kfree(resv);

		if (++freed >= XREP_REAP_MAX_NEWBT_EFIS) {
			error = xrep_defer_finish(sc);
			if (error)
				goto junkit;
			freed = 0;
		}
	}

	if (freed)
		error = xrep_defer_finish(sc);

junkit:
	xrep_newbt_free_resv(xnr);
	return error;
}

/* Feed one of the reserved btree blocks to the bulk loader. */
int
xrep_newbt_claim_block(
	struct xfs_btree_cur	*cur,
	struct xrep_newbt	*xnr,
	union xfs_btree_ptr	*ptr)
{
	struct xrep_newbt_resv	*resv;
	xfs_fsblock_t		fsb;

	/*
	 * The first item in the list should always have a free block unless
	 * we're completely out.
	 */
	resv = list_first_entry(&xnr->resv_list, struct xrep_newbt_resv, list);
	if (resv->used == resv->len)
		return -ENOSPC;

	/*
	 * Peel off a block from the start of the reservation.  We allocate
	 * blocks in order to place blocks on disk in increasing record or key
	 * order.  The block reservations tend to end up on the list in
	 * decreasing order, which hopefully results in leaf blocks ending up
	 * together.
	 */
	fsb = resv->fsbno + resv->used;
	resv->used++;

	/* If we used all the blocks in this reservation, move it to the end. */
	if (resv->used == resv->len)
		list_move_tail(&resv->list, &xnr->resv_list);

	trace_xrep_newbt_claim_block(cur->bc_mp,
			XFS_FSB_TO_AGNO(cur->bc_mp, fsb),
			XFS_FSB_TO_AGBNO(cur->bc_mp, fsb),
			1, xnr->oinfo.oi_owner);

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS)
		ptr->l = cpu_to_be64(fsb);
	else
		ptr->s = cpu_to_be32(XFS_FSB_TO_AGBNO(cur->bc_mp, fsb));
	return 0;
}

/*
 * Estimate proper slack values for a btree that's being reloaded.
 *
 * Under most circumstances, we'll take whatever default loading value the
 * btree bulk loading code calculates for us.  However, there are some
 * exceptions to this rule:
 *
 * (1) If someone turned one of the debug knobs.
 * (2) If this is a per-AG btree and the AG has less than ~9% space free.
 * (3) If this is an inode btree and the FS has less than ~9% space free.
 *
 * Note that we actually use 3/32 for the comparison to avoid division.
 */
void
xrep_bload_estimate_slack(
	struct xfs_scrub	*sc,
	struct xfs_btree_bload	*bload)
{
	uint64_t		free;
	uint64_t		sz;

	/*
	 * The xfs_globals values are set to -1 (i.e. take the bload defaults)
	 * unless someone has set them otherwise, so we just pull the values
	 * here.
	 */
	bload->leaf_slack = xfs_globals.bload_leaf_slack;
	bload->node_slack = xfs_globals.bload_node_slack;

	if (sc->ops->type == ST_PERAG) {
		free = sc->sa.pag->pagf_freeblks;
		sz = xfs_ag_block_count(sc->mp, sc->sa.pag->pag_agno);
	} else {
		free = percpu_counter_sum(&sc->mp->m_fdblocks);
		sz = sc->mp->m_sb.sb_dblocks;
	}

	/* No further changes if there's more than 3/32ths space left. */
	if (free >= ((sz * 3) >> 5))
		return;

	/* We're low on space; load the btrees as tightly as possible. */
	if (bload->leaf_slack < 0)
		bload->leaf_slack = 0;
	if (bload->node_slack < 0)
		bload->node_slack = 0;
}

/*
 * Reconstructing per-AG Btrees
 *
 * When a space btree is corrupt, we don't bother trying to fix it.  Instead,
 * we scan secondary space metadata to derive the records that should be in
 * the damaged btree, initialize a fresh btree root, and insert the records.
 * Note that for rebuilding the rmapbt we scan all the primary data to
 * generate the new records.
 *
 * However, that leaves the matter of removing all the metadata describing the
 * old broken structure.  For primary metadata we use the rmap data to collect
 * every extent with a matching rmap owner (bitmap); we then iterate all other
 * metadata structures with the same rmap owner to collect the extents that
 * cannot be removed (sublist).  We then subtract sublist from bitmap to
 * derive the blocks that were used by the old btree.  These blocks can be
 * reaped.
 *
 * For rmapbt reconstructions we must use different tactics for extent
 * collection.  First we iterate all primary metadata (this excludes the old
 * rmapbt, obviously) to generate new rmap records.  The gaps in the rmap
 * records are collected as bitmap.  The bnobt records are collected as
 * sublist.  As with the other btrees we subtract sublist from bitmap, and the
 * result (since the rmapbt lives in the free space) are the blocks from the
 * old rmapbt.
 *
 * Disposal of Blocks from Old per-AG Btrees
 *
 * Now that we've constructed a new btree to replace the damaged one, we want
 * to dispose of the blocks that (we think) the old btree was using.
 * Previously, we used the rmapbt to collect the extents (bitmap) with the
 * rmap owner corresponding to the tree we rebuilt, collected extents for any
 * blocks with the same rmap owner that are owned by another data structure
 * (sublist), and subtracted sublist from bitmap.  In theory the extents
 * remaining in bitmap are the old btree's blocks.
 *
 * Unfortunately, it's possible that the btree was crosslinked with other
 * blocks on disk.  The rmap data can tell us if there are multiple owners, so
 * if the rmapbt says there is an owner of this block other than @oinfo, then
 * the block is crosslinked.  Remove the reverse mapping and continue.
 *
 * If there is one rmap record, we can free the block, which removes the
 * reverse mapping but doesn't add the block to the free space.  Our repair
 * strategy is to hope the other metadata objects crosslinked on this block
 * will be rebuilt (atop different blocks), thereby removing all the cross
 * links.
 *
 * If there are no rmap records at all, we also free the block.  If the btree
 * being rebuilt lives in the free space (bnobt/cntbt/rmapbt) then there isn't
 * supposed to be a rmap record and everything is ok.  For other btrees there
 * had to have been an rmap entry for the block to have ended up on @bitmap,
 * so if it's gone now there's something wrong and the fs will shut down.
 *
 * Note: If there are multiple rmap records with only the same rmap owner as
 * the btree we're trying to rebuild and the block is indeed owned by another
 * data structure with the same rmap owner, then the block will be in sublist
 * and therefore doesn't need disposal.  If there are multiple rmap records
 * with only the same rmap owner but the block is not owned by something with
 * the same rmap owner, the block will be freed.
 *
 * The caller is responsible for locking the AG headers for the entire rebuild
 * operation so that nothing else can sneak in and change the AG state while
 * we're not looking.  We also assume that the caller already invalidated any
 * buffers associated with @bitmap.
 */

/* Ensure the freelist is the correct size. */
int
xrep_fix_freelist(
	struct xfs_scrub	*sc,
	bool			can_shrink)
{
	struct xfs_alloc_arg	args = {0};

	args.mp = sc->mp;
	args.tp = sc->tp;
	args.agno = sc->sa.pag->pag_agno;
	args.alignment = 1;
	args.pag = sc->sa.pag;

	return xfs_alloc_fix_freelist(&args,
			can_shrink ? 0 : XFS_ALLOC_FLAG_NOSHRINK);
}

/*
 * Put a block back on the AGFL.
 */
STATIC int
xrep_put_freelist(
	struct xfs_scrub	*sc,
	xfs_agblock_t		agbno)
{
	struct xfs_buf		*agfl_bp;
	int			error;

	/* Make sure there's space on the freelist. */
	error = xrep_fix_freelist(sc, true);
	if (error)
		return error;

	/*
	 * Since we're "freeing" a lost block onto the AGFL, we have to
	 * create an rmap for the block prior to merging it or else other
	 * parts will break.
	 */
	error = xfs_rmap_alloc(sc->tp, sc->sa.agf_bp, sc->sa.pag, agbno, 1,
			&XFS_RMAP_OINFO_AG);
	if (error)
		return error;

	/* Put the block on the AGFL. */
	error = xfs_alloc_read_agfl(sc->mp, sc->tp, sc->sa.pag->pag_agno,
			&agfl_bp);
	if (error)
		return error;

	error = xfs_alloc_put_freelist(sc->tp, sc->sa.agf_bp, agfl_bp,
			agbno, 0);
	if (error)
		return error;
	xfs_extent_busy_insert(sc->tp, sc->sa.pag, agbno, 1,
			XFS_EXTENT_BUSY_SKIP_DISCARD);

	return 0;
}

/* Information about reaping extents after a repair. */
struct xrep_reap_state {
	struct xfs_scrub		*sc;

	/* Reverse mapping owner and metadata reservation type. */
	const struct xfs_owner_info	*oinfo;
	enum xfs_ag_resv_type		resv;

	/* If true, roll the transaction before reaping the next extent. */
	bool				force_roll;

	/* Number of EFIs queued to the current transaction. */
	unsigned int			deferred;

	/* Number of invalidated buffers logged to the current transaction. */
	unsigned int			invalidated;
};

#define XREP_REAP_MAX_EFIS	(128)
#define XREP_REAP_MAX_BINVAL	(2048)

/*
 * Decide if we want to roll the transaction after reaping an extent.  We don't
 * want to overrun the transaction reservation, so we prohibit more than
 * 128 EFIs per transaction.  For the same reason, we limit the number
 * of buffer invalidations to 2048.
 */
static inline bool xrep_want_reap_roll(const struct xrep_reap_state *rs)
{
	if (rs->force_roll)
		return true;
	if (rs->deferred > XREP_REAP_MAX_EFIS)
		return true;
	if (rs->invalidated > XREP_REAP_MAX_BINVAL)
		return true;
	return false;
}

static inline void xrep_reap_reset(struct xrep_reap_state *rs)
{
	rs->deferred = 0;
	rs->invalidated = 0;
	rs->force_roll = false;
}

/* Try to invalidate the incore buffers for an extent that we're freeing. */
STATIC void
xrep_agextent_reap_binval(
	struct xrep_reap_state	*rs,
	xfs_agblock_t		agbno,
	xfs_extlen_t		*aglenp)
{
	struct xfs_scrub	*sc = rs->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_buf		*bp;
	xfs_agnumber_t		agno = sc->sa.pag->pag_agno;
	xfs_agblock_t		agbno_next = agbno + *aglenp;
	xfs_agblock_t		bno = agbno;

	/*
	 * Avoid invalidating AG headers and post-EOFS blocks because we never
	 * own those.
	 */
	if (!xfs_verify_agbno(mp, agno, agbno) ||
	    !xfs_verify_agbno(mp, agno, agbno_next - 1))
		return;

	/*
	 * If there are incore buffers for these blocks, invalidate them.  We
	 * assume that the lack of any other known owners means that the buffer
	 * can be locked without risk of deadlocking.  The buffer cache cannot
	 * detect aliasing, so employ nested loops to scan for incore buffers
	 * of any plausible size.
	 */
	while (bno < agbno_next) {
		xfs_agblock_t	fsbcount;
		xfs_agblock_t	max_fsbs;

		/*
		 * Max buffer size is the max remote xattr buffer size, which
		 * is one fs block larger than 64k.
		 */
		max_fsbs = min_t(xfs_agblock_t, agbno_next - bno,
				xfs_attr3_rmt_blocks(mp, XFS_XATTR_SIZE_MAX));

		for (fsbcount = 1; fsbcount < max_fsbs; fsbcount++) {
			xfs_daddr_t	daddr;

			daddr = XFS_AGB_TO_DADDR(mp, agno, bno);
			bp = xfs_buf_incore(mp->m_ddev_targp, daddr,
					XFS_FSB_TO_BB(mp, fsbcount),
					XBF_BCACHE_SCAN);
			if (!bp)
				continue;

			xfs_trans_bjoin(sc->tp, bp);
			xfs_trans_binval(sc->tp, bp);
			rs->invalidated++;

			/*
			 * Stop invalidating if we've hit the limit; we should
			 * still have enough reservation left to free however
			 * far we've gotten.
			 */
			if (rs->invalidated > XREP_REAP_MAX_BINVAL) {
				*aglenp -= agbno_next - bno;
				goto out;
			}
		}

		bno++;
	}

out:
	trace_xrep_agextent_reap_binval(sc->sa.pag, agbno, *aglenp);
}

/* Dispose of a single AG extent. */
STATIC int
xrep_agextent_reap(
	struct xrep_reap_state	*rs,
	xfs_agblock_t		agbno,
	xfs_extlen_t		*aglenp,
	bool			crosslinked)
{
	struct xfs_scrub	*sc = rs->sc;
	xfs_fsblock_t		fsbno;
	int			error = 0;

	fsbno = XFS_AGB_TO_FSB(sc->mp, sc->sa.pag->pag_agno, agbno);

	/*
	 * If there are other rmappings, this block is cross linked and must
	 * not be freed.  Remove the reverse mapping and move on.  Otherwise,
	 * we were the only owner of the block, so free the extent, which will
	 * also remove the rmap.
	 *
	 * XXX: XFS doesn't support detecting the case where a single block
	 * metadata structure is crosslinked with a multi-block structure
	 * because the buffer cache doesn't detect aliasing problems, so we
	 * can't fix 100% of crosslinking problems (yet).  The verifiers will
	 * blow on writeout, the filesystem will shut down, and the admin gets
	 * to run xfs_repair.
	 */
	if (crosslinked) {
		trace_xrep_dispose_unmap_extent(sc->sa.pag, agbno, *aglenp);

		rs->force_roll = true;
		return xfs_rmap_free(sc->tp, sc->sa.agf_bp, sc->sa.pag, agbno,
				*aglenp, rs->oinfo);
	}

	trace_xrep_dispose_free_extent(sc->sa.pag, agbno, *aglenp);

	/*
	 * Invalidate as many buffers as we can, starting at agbno.  If this
	 * function sets *aglenp to zero, the transaction is full of logged
	 * buffer invalidations, so we need to return early so that we can
	 * roll and retry.
	 */
	xrep_agextent_reap_binval(rs, agbno, aglenp);
	if (*aglenp == 0) {
		ASSERT(xrep_want_reap_roll(rs));
		return 0;
	}

	switch (rs->resv) {
	case XFS_AG_RESV_AGFL:
		ASSERT(*aglenp == 1);
		error = xrep_put_freelist(sc, agbno);
		rs->force_roll = true;
		break;
	case XFS_AG_RESV_IGNORE:
		/*
		 * bnobt/cntbt blocks are counted as free space, so we pass
		 * XFS_AG_RESV_IGNORE when reaping the old free space btree
		 * blocks to avoid changing fdblocks.
		 */
		error = __xfs_free_extent(sc->tp, fsbno, *aglenp, rs->oinfo,
				rs->resv, true);
		rs->force_roll = true;
		break;
	default:
		/*
		 * Use deferred frees to get rid of the old btree blocks to try
		 * to minimize the window in which we could crash and lose the
		 * old blocks.
		 */
		__xfs_free_extent_later(sc->tp, fsbno, *aglenp, rs->oinfo, true);
		rs->deferred++;
		break;
	}

	return error;
}

/*
 * Figure out the longest run of blocks that we can dispose of with a single
 * call.  Cross-linked blocks should have their reverse mappings removed, but
 * single-owner extents can be freed.  AGFL blocks can only be put back one at
 * a time.
 */
STATIC int
xrep_agextent_reap_find(
	struct xrep_reap_state	*rs,
	xfs_agblock_t		agbno,
	xfs_agblock_t		agbno_next,
	bool			*crosslinked,
	xfs_extlen_t		*aglenp)
{
	struct xfs_scrub	*sc = rs->sc;
	struct xfs_btree_cur	*cur;
	xfs_agblock_t		bno = agbno + 1;
	xfs_extlen_t		len = 1;
	int			error;

	/*
	 * Determine if there are any other rmap records covering the first
	 * block of this extent.  If so, the block is crosslinked.
	 */
	cur = xfs_rmapbt_init_cursor(sc->mp, sc->tp, sc->sa.agf_bp,
			sc->sa.pag);
	error = xfs_rmap_has_other_keys(cur, agbno, 1, rs->oinfo,
			crosslinked);
	if (error)
		goto out_cur;

	/* AGFL blocks can only be deal with one at a time. */
	if (rs->resv == XFS_AG_RESV_AGFL)
		goto out_found;

	/*
	 * Figure out how many of the subsequent blocks have the same crosslink
	 * status.
	 */
	while (bno < agbno_next) {
		bool		also_crosslinked;

		error = xfs_rmap_has_other_keys(cur, bno, 1, rs->oinfo,
				&also_crosslinked);
		if (error)
			goto out_cur;

		if (*crosslinked != also_crosslinked)
			break;

		len++;
		bno++;
	}

out_found:
	*aglenp = len;
	trace_xrep_agextent_reap_find(sc->sa.pag, agbno, len, *crosslinked);
out_cur:
	xfs_btree_del_cursor(cur, error);
	return error;
}

/*
 * Break an AG metadata extent into sub-extents by fate (crosslinked, not
 * crosslinked), and dispose of each sub-extent separately.
 */
STATIC int
xrep_agmeta_extent_reap(
	uint64_t		fsbno,
	uint64_t		len,
	void			*priv)
{
	struct xrep_reap_state	*rs = priv;
	struct xfs_scrub	*sc = rs->sc;
	xfs_agnumber_t		agno = XFS_FSB_TO_AGNO(sc->mp, fsbno);
	xfs_agblock_t		agbno = XFS_FSB_TO_AGBNO(sc->mp, fsbno);
	xfs_agblock_t		agbno_next = agbno + len;
	int			error = 0;

	ASSERT(len <= XFS_MAX_BMBT_EXTLEN);
	ASSERT(sc->ip == NULL);

	if (agno != sc->sa.pag->pag_agno) {
		ASSERT(sc->sa.pag->pag_agno == agno);
		return -EFSCORRUPTED;
	}

	while (agbno < agbno_next) {
		xfs_extlen_t	aglen;
		bool		crosslinked;

		error = xrep_agextent_reap_find(rs, agbno, agbno_next,
				&crosslinked, &aglen);
		if (error)
			return error;

		error = xrep_agextent_reap(rs, agbno, &aglen, crosslinked);
		if (error)
			return error;

		if (xrep_want_reap_roll(rs)) {
			error = xrep_roll_ag_trans(sc);
			if (error)
				return error;
			xrep_reap_reset(rs);
		}

		agbno += aglen;
	}

	return 0;
}

/*
 * Break a file metadata extent into sub-extents by fate (crosslinked, not
 * crosslinked), and dispose of each sub-extent separately.  The extent must
 * not cross an AG boundary.
 */
STATIC int
xrep_imeta_extent_reap(
	uint64_t		fsbno,
	uint64_t		len,
	void			*priv)
{
	struct xrep_reap_state	*rs = priv;
	struct xfs_scrub	*sc = rs->sc;
	xfs_agnumber_t		agno = XFS_FSB_TO_AGNO(sc->mp, fsbno);
	xfs_agblock_t		agbno = XFS_FSB_TO_AGBNO(sc->mp, fsbno);
	xfs_agblock_t		agbno_next = agbno + len;
	int			error = 0;

	ASSERT(len <= XFS_MAX_BMBT_EXTLEN);
	ASSERT(sc->ip != NULL);
	ASSERT(!sc->sa.pag);

	/*
	 * We're reaping blocks after repairing file metadata, which means that
	 * we have to init the xchk_ag structure ourselves.
	 */
	sc->sa.pag = xfs_perag_get(sc->mp, agno);
	if (!sc->sa.pag)
		return -EFSCORRUPTED;

	error = xfs_alloc_read_agf(sc->mp, sc->tp, agno, 0, &sc->sa.agf_bp);
	if (error)
		goto out_pag;

	while (agbno < agbno_next) {
		xfs_extlen_t	aglen;
		bool		crosslinked;

		error = xrep_agextent_reap_find(rs, agbno, agbno_next,
				&crosslinked, &aglen);
		if (error)
			goto out_agf;

		error = xrep_agextent_reap(rs, agbno, &aglen, crosslinked);
		if (error)
			goto out_agf;

		if (xrep_want_reap_roll(rs)) {
			/*
			 * Hold the AGF buffer across the transaction roll so
			 * that we don't have to reattach it to the scrub
			 * context.
			 */
			xfs_trans_bhold(sc->tp, sc->sa.agf_bp);
			error = xfs_trans_roll_inode(&sc->tp, sc->ip);
			xfs_trans_bjoin(sc->tp, sc->sa.agf_bp);
			if (error)
				goto out_agf;
			xrep_reap_reset(rs);
		}

		agbno += aglen;
	}

out_agf:
	xfs_trans_brelse(sc->tp, sc->sa.agf_bp);
	sc->sa.agf_bp = NULL;
out_pag:
	xfs_perag_put(sc->sa.pag);
	sc->sa.pag = NULL;
	return error;
}

/*
 * Dispose of every block of every extent in the bitmap.  Do not use this for
 * file data/attr extents.
 */
int
xrep_reap_extents(
	struct xfs_scrub		*sc,
	struct xbitmap			*bitmap,
	const struct xfs_owner_info	*oinfo,
	enum xfs_ag_resv_type		type)
{
	struct xrep_reap_state		rs = {
		.sc			= sc,
		.oinfo			= oinfo,
		.resv			= type,
	};
	int				error;

	ASSERT(xfs_has_rmapbt(sc->mp));

	if (sc->ip != NULL) {
		error = xbitmap_walk(bitmap, xrep_imeta_extent_reap, &rs);
		if (error || rs.deferred == 0)
			return error;

		return xfs_trans_roll_inode(&sc->tp, sc->ip);
	}

	error = xbitmap_walk(bitmap, xrep_agmeta_extent_reap, &rs);
	if (error || rs.deferred == 0)
		return error;

	return xrep_roll_ag_trans(sc);
}

/*
 * Finding per-AG Btree Roots for AGF/AGI Reconstruction
 *
 * If the AGF or AGI become slightly corrupted, it may be necessary to rebuild
 * the AG headers by using the rmap data to rummage through the AG looking for
 * btree roots.  This is not guaranteed to work if the AG is heavily damaged
 * or the rmap data are corrupt.
 *
 * Callers of xrep_find_ag_btree_roots must lock the AGF and AGFL
 * buffers if the AGF is being rebuilt; or the AGF and AGI buffers if the
 * AGI is being rebuilt.  It must maintain these locks until it's safe for
 * other threads to change the btrees' shapes.  The caller provides
 * information about the btrees to look for by passing in an array of
 * xrep_find_ag_btree with the (rmap owner, buf_ops, magic) fields set.
 * The (root, height) fields will be set on return if anything is found.  The
 * last element of the array should have a NULL buf_ops to mark the end of the
 * array.
 *
 * For every rmapbt record matching any of the rmap owners in btree_info,
 * read each block referenced by the rmap record.  If the block is a btree
 * block from this filesystem matching any of the magic numbers and has a
 * level higher than what we've already seen, remember the block and the
 * height of the tree required to have such a block.  When the call completes,
 * we return the highest block we've found for each btree description; those
 * should be the roots.
 */

struct xrep_findroot {
	struct xfs_scrub		*sc;
	struct xfs_buf			*agfl_bp;
	struct xfs_agf			*agf;
	struct xrep_find_ag_btree	*btree_info;
};

/* See if our block is in the AGFL. */
STATIC int
xrep_findroot_agfl_walk(
	struct xfs_mount	*mp,
	xfs_agblock_t		bno,
	void			*priv)
{
	xfs_agblock_t		*agbno = priv;

	return (*agbno == bno) ? -ECANCELED : 0;
}

/* Does this block match the btree information passed in? */
STATIC int
xrep_findroot_block(
	struct xrep_findroot		*ri,
	struct xrep_find_ag_btree	*fab,
	uint64_t			owner,
	xfs_agblock_t			agbno,
	bool				*done_with_block)
{
	struct xfs_mount		*mp = ri->sc->mp;
	struct xfs_buf			*bp;
	struct xfs_btree_block		*btblock;
	xfs_daddr_t			daddr;
	int				block_level;
	int				error = 0;

	daddr = XFS_AGB_TO_DADDR(mp, ri->sc->sa.pag->pag_agno, agbno);

	/*
	 * Blocks in the AGFL have stale contents that might just happen to
	 * have a matching magic and uuid.  We don't want to pull these blocks
	 * in as part of a tree root, so we have to filter out the AGFL stuff
	 * here.  If the AGFL looks insane we'll just refuse to repair.
	 */
	if (owner == XFS_RMAP_OWN_AG) {
		error = xfs_agfl_walk(mp, ri->agf, ri->agfl_bp,
				xrep_findroot_agfl_walk, &agbno);
		if (error == -ECANCELED)
			return 0;
		if (error)
			return error;
	}

	/*
	 * Read the buffer into memory so that we can see if it's a match for
	 * our btree type.  We have no clue if it is beforehand, and we want to
	 * avoid xfs_trans_read_buf's behavior of dumping the DONE state (which
	 * will cause needless disk reads in subsequent calls to this function)
	 * and logging metadata verifier failures.
	 *
	 * Therefore, pass in NULL buffer ops.  If the buffer was already in
	 * memory from some other caller it will already have b_ops assigned.
	 * If it was in memory from a previous unsuccessful findroot_block
	 * call, the buffer won't have b_ops but it should be clean and ready
	 * for us to try to verify if the read call succeeds.  The same applies
	 * if the buffer wasn't in memory at all.
	 *
	 * Note: If we never match a btree type with this buffer, it will be
	 * left in memory with NULL b_ops.  This shouldn't be a problem unless
	 * the buffer gets written.
	 */
	error = xfs_trans_read_buf(mp, ri->sc->tp, mp->m_ddev_targp, daddr,
			mp->m_bsize, 0, &bp, NULL);
	if (error)
		return error;

	/* Ensure the block magic matches the btree type we're looking for. */
	btblock = XFS_BUF_TO_BLOCK(bp);
	ASSERT(fab->buf_ops->magic[1] != 0);
	if (btblock->bb_magic != fab->buf_ops->magic[1])
		goto out;

	/*
	 * If the buffer already has ops applied and they're not the ones for
	 * this btree type, we know this block doesn't match the btree and we
	 * can bail out.
	 *
	 * If the buffer ops match ours, someone else has already validated
	 * the block for us, so we can move on to checking if this is a root
	 * block candidate.
	 *
	 * If the buffer does not have ops, nobody has successfully validated
	 * the contents and the buffer cannot be dirty.  If the magic, uuid,
	 * and structure match this btree type then we'll move on to checking
	 * if it's a root block candidate.  If there is no match, bail out.
	 */
	if (bp->b_ops) {
		if (bp->b_ops != fab->buf_ops)
			goto out;
	} else {
		ASSERT(!xfs_trans_buf_is_dirty(bp));
		if (!uuid_equal(&btblock->bb_u.s.bb_uuid,
				&mp->m_sb.sb_meta_uuid))
			goto out;
		/*
		 * Read verifiers can reference b_ops, so we set the pointer
		 * here.  If the verifier fails we'll reset the buffer state
		 * to what it was before we touched the buffer.
		 */
		bp->b_ops = fab->buf_ops;
		fab->buf_ops->verify_read(bp);
		if (bp->b_error) {
			bp->b_ops = NULL;
			bp->b_error = 0;
			goto out;
		}

		/*
		 * Some read verifiers will (re)set b_ops, so we must be
		 * careful not to change b_ops after running the verifier.
		 */
	}

	/*
	 * This block passes the magic/uuid and verifier tests for this btree
	 * type.  We don't need the caller to try the other tree types.
	 */
	*done_with_block = true;

	/*
	 * Compare this btree block's level to the height of the current
	 * candidate root block.
	 *
	 * If the level matches the root we found previously, throw away both
	 * blocks because there can't be two candidate roots.
	 *
	 * If level is lower in the tree than the root we found previously,
	 * ignore this block.
	 */
	block_level = xfs_btree_get_level(btblock);
	if (block_level + 1 == fab->height) {
		fab->root = NULLAGBLOCK;
		goto out;
	} else if (block_level < fab->height) {
		goto out;
	}

	/*
	 * This is the highest block in the tree that we've found so far.
	 * Update the btree height to reflect what we've learned from this
	 * block.
	 */
	fab->height = block_level + 1;

	/*
	 * If this block doesn't have sibling pointers, then it's the new root
	 * block candidate.  Otherwise, the root will be found farther up the
	 * tree.
	 */
	if (btblock->bb_u.s.bb_leftsib == cpu_to_be32(NULLAGBLOCK) &&
	    btblock->bb_u.s.bb_rightsib == cpu_to_be32(NULLAGBLOCK))
		fab->root = agbno;
	else
		fab->root = NULLAGBLOCK;

	trace_xrep_findroot_block(mp, ri->sc->sa.pag->pag_agno, agbno,
			be32_to_cpu(btblock->bb_magic), fab->height - 1);
out:
	xfs_trans_brelse(ri->sc->tp, bp);
	return error;
}

/*
 * Do any of the blocks in this rmap record match one of the btrees we're
 * looking for?
 */
STATIC int
xrep_findroot_rmap(
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec,
	void				*priv)
{
	struct xrep_findroot		*ri = priv;
	struct xrep_find_ag_btree	*fab;
	xfs_agblock_t			b;
	bool				done;
	int				error = 0;

	/* Ignore anything that isn't AG metadata. */
	if (!XFS_RMAP_NON_INODE_OWNER(rec->rm_owner))
		return 0;

	/* Otherwise scan each block + btree type. */
	for (b = 0; b < rec->rm_blockcount; b++) {
		done = false;
		for (fab = ri->btree_info; fab->buf_ops; fab++) {
			if (rec->rm_owner != fab->rmap_owner)
				continue;
			error = xrep_findroot_block(ri, fab,
					rec->rm_owner, rec->rm_startblock + b,
					&done);
			if (error)
				return error;
			if (done)
				break;
		}
	}

	return 0;
}

/* Find the roots of the per-AG btrees described in btree_info. */
int
xrep_find_ag_btree_roots(
	struct xfs_scrub		*sc,
	struct xfs_buf			*agf_bp,
	struct xrep_find_ag_btree	*btree_info,
	struct xfs_buf			*agfl_bp)
{
	struct xfs_mount		*mp = sc->mp;
	struct xrep_findroot		ri;
	struct xrep_find_ag_btree	*fab;
	struct xfs_btree_cur		*cur;
	int				error;

	ASSERT(xfs_buf_islocked(agf_bp));
	ASSERT(agfl_bp == NULL || xfs_buf_islocked(agfl_bp));

	ri.sc = sc;
	ri.btree_info = btree_info;
	ri.agf = agf_bp->b_addr;
	ri.agfl_bp = agfl_bp;
	for (fab = btree_info; fab->buf_ops; fab++) {
		ASSERT(agfl_bp || fab->rmap_owner != XFS_RMAP_OWN_AG);
		ASSERT(XFS_RMAP_NON_INODE_OWNER(fab->rmap_owner));
		fab->root = NULLAGBLOCK;
		fab->height = 0;
	}

	cur = xfs_rmapbt_init_cursor(mp, sc->tp, agf_bp, sc->sa.pag);
	error = xfs_rmap_query_all(cur, xrep_findroot_rmap, &ri);
	xfs_btree_del_cursor(cur, error);

	return error;
}

/* Update some quota flags in the superblock. */
void
xrep_update_qflags(
	struct xfs_scrub	*sc,
	unsigned int		clear_flags,
	unsigned int		set_flags)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_buf		*bp;

	mutex_lock(&mp->m_quotainfo->qi_quotaofflock);
	if ((mp->m_qflags & clear_flags) == 0 &&
	    (mp->m_qflags & set_flags) == set_flags)
		goto no_update;

	mp->m_qflags &= ~clear_flags;
	mp->m_qflags |= set_flags;

	spin_lock(&mp->m_sb_lock);
	mp->m_sb.sb_qflags &= ~clear_flags;
	mp->m_sb.sb_qflags |= set_flags;
	spin_unlock(&mp->m_sb_lock);

	/*
	 * Update the quota flags in the ondisk superblock without touching
	 * the summary counters.  We have not quiesced inode chunk allocation,
	 * so we cannot coordinate with updates to the icount and ifree percpu
	 * counters.
	 */
	bp = xfs_trans_getsb(sc->tp);
	xfs_sb_to_disk(bp->b_addr, &mp->m_sb);
	xfs_trans_buf_set_type(sc->tp, bp, XFS_BLFT_SB_BUF);
	xfs_trans_log_buf(sc->tp, bp, 0, sizeof(struct xfs_dsb) - 1);

no_update:
	mutex_unlock(&sc->mp->m_quotainfo->qi_quotaofflock);
}

/* Force a quotacheck the next time we mount. */
void
xrep_force_quotacheck(
	struct xfs_scrub	*sc,
	xfs_dqtype_t		type)
{
	uint			flag;

	flag = xfs_quota_chkd_flag(type);
	if (!(flag & sc->mp->m_qflags))
		return;

	xrep_update_qflags(sc, flag, 0);
}

/*
 * Attach dquots to this inode, or schedule quotacheck to fix them.
 *
 * This function ensures that the appropriate dquots are attached to an inode.
 * We cannot allow the dquot code to allocate an on-disk dquot block here
 * because we're already in transaction context with the inode locked.  The
 * on-disk dquot should already exist anyway.  If the quota code signals
 * corruption or missing quota information, schedule quotacheck, which will
 * repair corruptions in the quota metadata.
 */
int
xrep_ino_dqattach(
	struct xfs_scrub	*sc)
{
	int			error;

	error = xfs_qm_dqattach_locked(sc->ip, false);
	switch (error) {
	case -EFSBADCRC:
	case -EFSCORRUPTED:
	case -ENOENT:
		xfs_err_ratelimited(sc->mp,
"inode %llu repair encountered quota error %d, quotacheck forced.",
				(unsigned long long)sc->ip->i_ino, error);
		if (XFS_IS_UQUOTA_ON(sc->mp) && !sc->ip->i_udquot)
			xrep_force_quotacheck(sc, XFS_DQTYPE_USER);
		if (XFS_IS_GQUOTA_ON(sc->mp) && !sc->ip->i_gdquot)
			xrep_force_quotacheck(sc, XFS_DQTYPE_GROUP);
		if (XFS_IS_PQUOTA_ON(sc->mp) && !sc->ip->i_pdquot)
			xrep_force_quotacheck(sc, XFS_DQTYPE_PROJ);
		fallthrough;
	case -ESRCH:
		error = 0;
		break;
	default:
		break;
	}

	return error;
}

/*
 * Ensure that the inode being repaired is ready to handle a certain number of
 * extents, or return EFSCORRUPTED.  Caller must hold the ILOCK of the inode
 * being repaired and have joined it to the scrub transaction.
 */
int
xrep_ino_ensure_extent_count(
	struct xfs_scrub	*sc,
	int			whichfork,
	xfs_extnum_t		nextents)
{
	xfs_extnum_t		max_extents;
	bool			large_extcount;

	large_extcount = xfs_inode_has_large_extent_counts(sc->ip);
	max_extents = xfs_iext_max_nextents(large_extcount, whichfork);
	if (nextents <= max_extents)
		return 0;
	if (large_extcount)
		return -EFSCORRUPTED;
	if (!xfs_has_large_extent_counts(sc->mp))
		return -EFSCORRUPTED;

	max_extents = xfs_iext_max_nextents(true, whichfork);
	if (nextents > max_extents)
		return -EFSCORRUPTED;

	sc->ip->i_diflags2 |= XFS_DIFLAG2_NREXT64;
	xfs_trans_log_inode(sc->tp, sc->ip, XFS_ILOG_CORE);
	return 0;
}

/* Initialize all the btree cursors for an AG repair. */
void
xrep_ag_btcur_init(
	struct xfs_scrub	*sc,
	struct xchk_ag		*sa)
{
	struct xfs_mount	*mp = sc->mp;

	/* Set up a bnobt cursor for cross-referencing. */
	if (sc->sm->sm_type != XFS_SCRUB_TYPE_BNOBT &&
	    sc->sm->sm_type != XFS_SCRUB_TYPE_CNTBT) {
		sa->bno_cur = xfs_allocbt_init_cursor(mp, sc->tp, sa->agf_bp,
				sc->sa.pag, XFS_BTNUM_BNO);
		sa->cnt_cur = xfs_allocbt_init_cursor(mp, sc->tp, sa->agf_bp,
				sc->sa.pag, XFS_BTNUM_CNT);
	}

	/* Set up a inobt cursor for cross-referencing. */
	if (sc->sm->sm_type != XFS_SCRUB_TYPE_INOBT &&
	    sc->sm->sm_type != XFS_SCRUB_TYPE_FINOBT) {
		sa->ino_cur = xfs_inobt_init_cursor(mp, sc->tp, sa->agi_bp,
				sc->sa.pag, XFS_BTNUM_INO);
		if (xfs_has_finobt(mp))
			sa->fino_cur = xfs_inobt_init_cursor(mp, sc->tp,
					sa->agi_bp, sc->sa.pag, XFS_BTNUM_FINO);
	}

	/* Set up a rmapbt cursor for cross-referencing. */
	if (sc->sm->sm_type != XFS_SCRUB_TYPE_RMAPBT &&
	    xfs_has_rmapbt(mp))
		sa->rmap_cur = xfs_rmapbt_init_cursor(mp, sc->tp, sa->agf_bp,
				sc->sa.pag);

	/* Set up a refcountbt cursor for cross-referencing. */
	if (sc->sm->sm_type != XFS_SCRUB_TYPE_REFCNTBT &&
	    xfs_has_reflink(mp))
		sa->refc_cur = xfs_refcountbt_init_cursor(mp, sc->tp,
				sa->agf_bp, sc->sa.pag);
}

/* Given a reference to a perag structure, load AG headers and cursors. */
int
xrep_ag_init(
	struct xfs_scrub	*sc,
	struct xfs_perag	*pag,
	struct xchk_ag		*sa)
{
	int			error;

	ASSERT(!sa->pag);

	error = xfs_ialloc_read_agi(sc->mp, sc->tp, pag->pag_agno,
			&sa->agi_bp);
	if (error)
		return error;

	error = xfs_alloc_read_agf(sc->mp, sc->tp, pag->pag_agno, 0,
			&sa->agf_bp);
	if (error)
		return error;

	/* Grab our own reference to the perag structure. */
	atomic_inc(&pag->pag_ref);
	sa->pag = pag;
	xrep_ag_btcur_init(sc, sa);
	return 0;
}

/* Reinitialize the per-AG block reservation for the AG we just fixed. */
int
xrep_reset_perag_resv(
	struct xfs_scrub	*sc)
{
	int			error;

	if (!(sc->flags & XREP_RESET_PERAG_RESV))
		return 0;

	ASSERT(sc->sa.pag != NULL);
	ASSERT(sc->ops->type == ST_PERAG);
	ASSERT(sc->tp);

	sc->flags &= ~XREP_RESET_PERAG_RESV;
	error = xfs_ag_resv_free(sc->sa.pag);
	if (error)
		goto out;
	error = xfs_ag_resv_init(sc->sa.pag, sc->tp);
	if (error == -ENOSPC) {
		xfs_err(sc->mp,
"Insufficient free space to reset per-AG reservation for AG %u after repair.",
				sc->sa.pag->pag_agno);
		error = 0;
	}

out:
	return error;
}

/* Decide if we are going to call the repair function for a scrub type. */
bool
xrep_will_attempt(
	struct xfs_scrub	*sc)
{
	/* Userspace asked us to rebuild the structure regardless. */
	if (sc->sm->sm_flags & XFS_SCRUB_IFLAG_FORCE_REBUILD)
		return true;

	/* Let debug users force us into the repair routines. */
	if (XFS_TEST_ERROR(false, sc->mp, XFS_ERRTAG_FORCE_SCRUB_REPAIR))
		return true;

	/* Metadata is corrupt or failed cross-referencing. */
	if (xchk_needs_repair(sc->sm))
		return true;

	return false;
}

/* Try to fix some part of a metadata inode by calling another scrubber. */
STATIC int
xrep_metadata_inode_subtype(
	struct xfs_scrub	*sc,
	unsigned int		scrub_type)
{
	__u32			smtype = sc->sm->sm_type;
	__u32			smflags = sc->sm->sm_flags;
	int			error;

	/*
	 * Let's see if the inode needs repair.  We're going to open-code calls
	 * to the scrub and repair functions so that we can hang on to the
	 * resources that we already acquired instead of using the standard
	 * setup/teardown routines.
	 */
	sc->sm->sm_flags &= ~XFS_SCRUB_FLAGS_OUT;
	sc->sm->sm_type = scrub_type;

	switch (scrub_type) {
	case XFS_SCRUB_TYPE_INODE:
		error = xchk_inode(sc);
		break;
	case XFS_SCRUB_TYPE_BMBTD:
		error = xchk_bmap_data(sc);
		break;
	case XFS_SCRUB_TYPE_BMBTA:
		error = xchk_bmap_attr(sc);
		break;
	default:
		ASSERT(0);
		error = -EFSCORRUPTED;
	}
	if (error)
		goto out;

	if (!xrep_will_attempt(sc))
		goto out;

	/*
	 * Repair some part of the inode.  This will potentially join the inode
	 * to the transaction.
	 */
	switch (scrub_type) {
	case XFS_SCRUB_TYPE_INODE:
		error = xrep_inode(sc);
		break;
	case XFS_SCRUB_TYPE_BMBTD:
		error = xrep_bmap(sc, XFS_DATA_FORK, false);
		break;
	case XFS_SCRUB_TYPE_BMBTA:
		error = xrep_bmap(sc, XFS_ATTR_FORK, false);
		break;
	}
	if (error)
		goto out;

	/*
	 * Finish all deferred intent items and then roll the transaction so
	 * that the inode will not be joined to the transaction when we exit
	 * the function.
	 */
	error = xfs_defer_finish(&sc->tp);
	if (error)
		goto out;
	error = xfs_trans_roll(&sc->tp);
	if (error)
		goto out;

	/*
	 * Clear the corruption flags and re-check the metadata that we just
	 * repaired.
	 */
	sc->sm->sm_flags &= ~XFS_SCRUB_FLAGS_OUT;

	switch (scrub_type) {
	case XFS_SCRUB_TYPE_INODE:
		error = xchk_inode(sc);
		break;
	case XFS_SCRUB_TYPE_BMBTD:
		error = xchk_bmap_data(sc);
		break;
	case XFS_SCRUB_TYPE_BMBTA:
		error = xchk_bmap_attr(sc);
		break;
	}
	if (error)
		goto out;

	/* If corruption persists, the repair has failed. */
	if (xchk_needs_repair(sc->sm)) {
		error = -EFSCORRUPTED;
		goto out;
	}
out:
	sc->sm->sm_type = smtype;
	sc->sm->sm_flags = smflags;
	return error;
}

/*
 * Repair the ondisk forks of a metadata inode.  The caller must ensure that
 * sc->ip points to the metadata inode and the ILOCK is held on that inode.
 * The inode must not be joined to the transaction before the call, and will
 * not be afterwards.
 */
int
xrep_metadata_inode_forks(
	struct xfs_scrub	*sc)
{
	bool			dirty = false;
	int			error;

	/* Repair the inode record and the data fork. */
	error = xrep_metadata_inode_subtype(sc, XFS_SCRUB_TYPE_INODE);
	if (error)
		return error;

	error = xrep_metadata_inode_subtype(sc, XFS_SCRUB_TYPE_BMBTD);
	if (error)
		return error;

	/* Make sure the attr fork looks ok before we delete it. */
	error = xrep_metadata_inode_subtype(sc, XFS_SCRUB_TYPE_BMBTA);
	if (error)
		return error;

	/* Clear the reflink flag since metadata never shares. */
	if (xfs_is_reflink_inode(sc->ip)) {
		dirty = true;
		xfs_trans_ijoin(sc->tp, sc->ip, 0);
		error = xfs_reflink_clear_inode_flag(sc->ip, &sc->tp);
		if (error)
			return error;
	}

	/*
	 * If we modified the inode, roll the transaction but don't rejoin the
	 * inode to the new transaction because xrep_bmap_data can do that.
	 */
	if (dirty) {
		error = xfs_trans_roll(&sc->tp);
		if (error)
			return error;
		dirty = false;
	}

	return 0;
}
