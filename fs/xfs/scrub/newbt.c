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
#include "xfs_btree.h"
#include "xfs_btree_staging.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_log.h"
#include "xfs_sb.h"
#include "xfs_inode.h"
#include "xfs_alloc.h"
#include "xfs_rmap.h"
#include "xfs_ag.h"
#include "xfs_defer.h"
#include "xfs_extfree_item.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/newbt.h"

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
static void
xrep_newbt_estimate_slack(
	struct xrep_newbt	*xnr)
{
	struct xfs_scrub	*sc = xnr->sc;
	struct xfs_btree_bload	*bload = &xnr->bload;
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
	xrep_newbt_estimate_slack(xnr);
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
	xnr->ifake.if_fork_size = xfs_inode_fork_size(sc->ip, whichfork);
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
