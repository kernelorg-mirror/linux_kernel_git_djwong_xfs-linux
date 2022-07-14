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
#include "xfs_sb.h"
#include "xfs_inode.h"
#include "xfs_alloc.h"
#include "xfs_rmap.h"
#include "xfs_ag.h"
#include "xfs_defer.h"
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

	/* Let the btree code compute the default slack values. */
	bload->leaf_slack = -1;
	bload->node_slack = -1;

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

/* Designate specific blocks to be used to build our new btree. */
int
xrep_newbt_add_blocks(
	struct xrep_newbt		*xnr,
	xfs_fsblock_t			fsbno,
	xfs_extlen_t			len)
{
	struct xrep_newbt_resv		*resv;
	struct xfs_mount		*mp = xnr->sc->mp;

	resv = kmalloc(sizeof(struct xrep_newbt_resv), XCHK_GFP_FLAGS);
	if (!resv)
		return -ENOMEM;

	INIT_LIST_HEAD(&resv->list);
	resv->agbno = XFS_FSB_TO_AGBNO(mp, fsbno);
	resv->len = len;
	resv->used = 0;
	resv->pag = xfs_perag_get(mp, XFS_FSB_TO_AGNO(mp, fsbno));

	list_add_tail(&resv->list, &xnr->resv_list);
	return 0;
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

		error = xrep_newbt_add_blocks(xnr, args.fsbno, args.len);
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

/*
 * How many extent freeing items can we attach to a transaction before we want
 * to finish the chain so that unreserving new btree blocks doesn't overrun
 * the transaction reservation?
 */
#define XREP_REAP_MAX_NEWBT_EFIS	(128)

/*
 * Free the unused part of an extent.  Returns the number of EFIs logged or
 * a negative errno.
 */
STATIC int
xrep_newbt_free_extent(
	struct xrep_newbt	*xnr,
	struct xrep_newbt_resv	*resv,
	bool			btree_committed)
{
	struct xfs_scrub	*sc = xnr->sc;
	xfs_agblock_t		free_agbno = resv->agbno;
	xfs_extlen_t		free_aglen = resv->len;
	xfs_fsblock_t		fsbno;
	int			error;

	/* If we don't commit the new btree, free all of the space. */
	if (btree_committed) {
		free_agbno += resv->used;
		free_aglen -= resv->used;
	}

	if (free_aglen == 0)
		return 0;

	trace_xrep_newbt_free_blocks(sc->mp, resv->pag->pag_agno, free_agbno,
			free_aglen, xnr->oinfo.oi_owner);

	if (xnr->resv == XFS_AG_RESV_NONE) {
		/*
		 * No per-AG reservation means that we can use EFIs to free the
		 * reservations.  This reduces the chance that we leak blocks
		 * if the system goes down.
		 */
		fsbno = XFS_AGB_TO_FSB(sc->mp, resv->pag->pag_agno, free_agbno);
		__xfs_free_extent_later(sc->tp, fsbno, free_aglen, &xnr->oinfo,
				true);
		return 1;
	}

	if (xnr->resv == XFS_AG_RESV_RMAPBT ||
	    xnr->resv == XFS_AG_RESV_METADATA) {
		/*
		 * Metadata blocks taken from a per-AG reservation must be put
		 * back into that reservation immediately because EFIs cannot
		 * free into per-AG reservations.
		 */
		error = __xfs_free_extent(sc->tp, resv->pag, free_agbno,
				free_aglen, &xnr->oinfo, xnr->resv, true);
		if (error < 0)
			return error;
		return XREP_REAP_MAX_NEWBT_EFIS;
	}

	ASSERT(0);
	return -EFSCORRUPTED;
}

/* Free all the accounting info and disk space we reserved for a new btree. */
STATIC int
xrep_newbt_free(
	struct xrep_newbt	*xnr,
	bool			btree_committed)
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
		int		ret;

		ret = xrep_newbt_free_extent(xnr, resv, btree_committed);
		list_del(&resv->list);
		xfs_perag_put(resv->pag);
		kfree(resv);
		if (ret < 0) {
			error = ret;
			goto junkit;
		}

		freed += ret;
		if (freed >= XREP_REAP_MAX_NEWBT_EFIS) {
			error = xrep_defer_finish(sc);
			if (error)
				goto junkit;
			freed = 0;
		}
	}

	if (freed)
		error = xrep_defer_finish(sc);

junkit:
	/*
	 * If we still have reservations attached to @newbt, cleanup must have
	 * failed and the filesystem is about to go down.  Clean up the incore
	 * reservations.
	 */
	list_for_each_entry_safe(resv, n, &xnr->resv_list, list) {
		list_del(&resv->list);
		xfs_perag_put(resv->pag);
		kfree(resv);
	}

	if (sc->ip) {
		kmem_cache_free(xfs_ifork_cache, xnr->ifake.if_fork);
		xnr->ifake.if_fork = NULL;
	}

	return error;
}

/*
 * Free all the accounting info and unused disk space allocations after
 * committing a new btree.
 */
int
xrep_newbt_commit(
	struct xrep_newbt	*xnr)
{
	return xrep_newbt_free(xnr, true);
}

/*
 * Free all the accounting info and all of the disk space we reserved for a new
 * btree that we're not going to commit.  We want to try to roll things back
 * cleanly for things like ENOSPC midway through allocation.
 */
void
xrep_newbt_cancel(
	struct xrep_newbt	*xnr)
{
	xrep_newbt_free(xnr, false);
}

/* Feed one of the reserved btree blocks to the bulk loader. */
int
xrep_newbt_claim_block(
	struct xfs_btree_cur	*cur,
	struct xrep_newbt	*xnr,
	union xfs_btree_ptr	*ptr)
{
	struct xrep_newbt_resv	*resv;
	struct xfs_mount	*mp = cur->bc_mp;
	xfs_agblock_t		agbno;

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
	agbno = resv->agbno + resv->used;
	resv->used++;

	/* If we used all the blocks in this reservation, move it to the end. */
	if (resv->used == resv->len)
		list_move_tail(&resv->list, &xnr->resv_list);

	trace_xrep_newbt_claim_block(mp, resv->pag->pag_agno, agbno, 1,
			xnr->oinfo.oi_owner);

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS)
		ptr->l = cpu_to_be64(XFS_AGB_TO_FSB(mp, resv->pag->pag_agno,
								agbno));
	else
		ptr->s = cpu_to_be32(agbno);
	return 0;
}
