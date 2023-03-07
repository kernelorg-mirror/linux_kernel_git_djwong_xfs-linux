// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2023 Oracle.  All Rights Reserved.
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
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_inode.h"
#include "xfs_rtalloc.h"
#include "xfs_rtgroup.h"
#include "xfs_imeta.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"

/* Set us up with the realtime metadata locked. */
int
xchk_setup_rtrmapbt(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_rtgroup	*rtg;
	int			error = 0;

	if (xchk_need_intent_drain(sc))
		xchk_fsgates_enable(sc, XCHK_FSGATES_DRAIN);

	rtg = xfs_rtgroup_get(mp, sc->sm->sm_agno);
	if (!rtg)
		return -ENOENT;

	error = xchk_setup_rt(sc);
	if (error)
		goto out_rtg;

	error = xchk_install_live_inode(sc, rtg->rtg_rmapip);
	if (error)
		goto out_rtg;

	error = xchk_ino_dqattach(sc);
	if (error)
		goto out_rtg;

	error = xchk_rtgroup_init(sc, rtg->rtg_rgno, &sc->sr, XCHK_RTGLOCK_ALL);
out_rtg:
	xfs_rtgroup_put(rtg);
	return error;
}

/* Realtime reverse mapping. */

struct xchk_rtrmap {
	/*
	 * The furthest-reaching of the rmapbt records that we've already
	 * processed.  This enables us to detect overlapping records for space
	 * allocations that cannot be shared.
	 */
	struct xfs_rmap_irec	overlap_rec;

	/*
	 * The previous rmapbt record, so that we can check for two records
	 * that could be one.
	 */
	struct xfs_rmap_irec	prev_rec;
};

/* Flag failures for records that overlap but cannot. */
STATIC void
xchk_rtrmapbt_check_overlapping(
	struct xchk_btree		*bs,
	struct xchk_rtrmap		*cr,
	const struct xfs_rmap_irec	*irec)
{
	xfs_rtblock_t			pnext, inext;

	if (bs->sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;

	/* No previous record? */
	if (cr->overlap_rec.rm_blockcount == 0)
		goto set_prev;

	/* Do overlap_rec and irec overlap? */
	pnext = cr->overlap_rec.rm_startblock + cr->overlap_rec.rm_blockcount;
	if (pnext <= irec->rm_startblock)
		goto set_prev;

	xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	/* Save whichever rmap record extends furthest. */
	inext = irec->rm_startblock + irec->rm_blockcount;
	if (pnext > inext)
		return;

set_prev:
	memcpy(&cr->overlap_rec, irec, sizeof(struct xfs_rmap_irec));
}

/* Decide if two reverse-mapping records can be merged. */
static inline bool
xchk_rtrmap_mergeable(
	struct xchk_rtrmap		*cr,
	const struct xfs_rmap_irec	*r2)
{
	const struct xfs_rmap_irec	*r1 = &cr->prev_rec;

	/* Ignore if prev_rec is not yet initialized. */
	if (cr->prev_rec.rm_blockcount == 0)
		return false;

	if (r1->rm_owner != r2->rm_owner)
		return false;
	if (r1->rm_startblock + r1->rm_blockcount != r2->rm_startblock)
		return false;
	if ((unsigned long long)r1->rm_blockcount + r2->rm_blockcount >
	    XFS_RMAP_LEN_MAX)
		return false;
	if (r1->rm_flags != r2->rm_flags)
		return false;
	return r1->rm_offset + r1->rm_blockcount == r2->rm_offset;
}

/* Flag failures for records that could be merged. */
STATIC void
xchk_rtrmapbt_check_mergeable(
	struct xchk_btree		*bs,
	struct xchk_rtrmap		*cr,
	const struct xfs_rmap_irec	*irec)
{
	if (bs->sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;

	if (xchk_rtrmap_mergeable(cr, irec))
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	memcpy(&cr->prev_rec, irec, sizeof(struct xfs_rmap_irec));
}

/* Cross-reference with other metadata. */
STATIC void
xchk_rtrmapbt_xref(
	struct xfs_scrub	*sc,
	struct xfs_rmap_irec	*irec)
{
	xfs_rtblock_t		rtbno;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;

	rtbno = xfs_rgbno_to_rtb(sc->mp, sc->sr.rtg->rtg_rgno,
			irec->rm_startblock);

	xchk_xref_is_used_rt_space(sc, rtbno, irec->rm_blockcount);
}

/* Scrub a realtime rmapbt record. */
STATIC int
xchk_rtrmapbt_rec(
	struct xchk_btree		*bs,
	const union xfs_btree_rec	*rec)
{
	struct xchk_rtrmap		*cr = bs->private;
	struct xfs_rmap_irec		irec;

	if (xfs_rmap_btrec_to_irec(rec, &irec) != NULL ||
	    xfs_rmap_check_irec(bs->cur, &irec) != NULL) {
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);
		return 0;
	}

	if (bs->sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return 0;

	xchk_rtrmapbt_check_mergeable(bs, cr, &irec);
	xchk_rtrmapbt_check_overlapping(bs, cr, &irec);
	xchk_rtrmapbt_xref(bs->sc, &irec);
	return 0;
}

/* Scrub the realtime rmap btree. */
int
xchk_rtrmapbt(
	struct xfs_scrub	*sc)
{
	struct xfs_owner_info	oinfo;
	struct xchk_rtrmap	cr = { };
	int			error;

	error = xchk_metadata_inode_forks(sc);
	if (error || (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT))
		return error;

	xfs_rmap_ino_bmbt_owner(&oinfo, sc->sr.rtg->rtg_rmapip->i_ino,
			XFS_DATA_FORK);
	return xchk_btree(sc, sc->sr.rmap_cur, xchk_rtrmapbt_rec, &oinfo, &cr);
}
