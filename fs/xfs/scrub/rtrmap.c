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
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_inode.h"
#include "xfs_rtalloc.h"
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
	int			error = 0;

	error = xchk_setup_fs(sc);
	if (error)
		return error;

	error = xchk_install_live_inode(sc, mp->m_rrmapip);
	if (error)
		return error;

	return xchk_rt_init(sc, &sc->sr);
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
	    XFS_RTRMAP_LEN_MAX)
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

/* Scrub a realtime rmapbt record. */
STATIC int
xchk_rtrmapbt_rec(
	struct xchk_btree		*bs,
	const union xfs_btree_rec	*rec)
{
	struct xfs_mount		*mp = bs->cur->bc_mp;
	struct xchk_rtrmap		*cr = bs->private;
	struct xfs_rmap_irec		irec;
	bool				non_inode;
	bool				is_bmbt;
	bool				is_attr;
	int				error;

	error = xfs_rmap_btrec_to_irec(bs->cur, rec, &irec);
	if (!xchk_btree_process_error(bs->sc, bs->cur, 0, &error))
		goto out;

	if (irec.rm_startblock + irec.rm_blockcount <= irec.rm_startblock ||
	    (!xfs_verify_rtbno(mp, irec.rm_startblock) ||
	     !xfs_verify_rtbno(mp, irec.rm_startblock +
				irec.rm_blockcount - 1)))
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	/* Check flags. */
	non_inode = XFS_RMAP_NON_INODE_OWNER(irec.rm_owner);
	is_bmbt = irec.rm_flags & XFS_RMAP_BMBT_BLOCK;
	is_attr = irec.rm_flags & XFS_RMAP_ATTR_FORK;

	if (is_bmbt || non_inode || is_attr)
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	if (!xfs_verify_ino(mp, irec.rm_owner))
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	if (bs->sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return 0;

	xchk_rtrmapbt_check_mergeable(bs, cr, &irec);
	xchk_rtrmapbt_check_overlapping(bs, cr, &irec);
out:
	return error;
}

/* Scrub the realtime rmap btree. */
int
xchk_rtrmapbt(
	struct xfs_scrub	*sc)
{
	struct xfs_owner_info	oinfo;
	struct xchk_rtrmap	cr = { };
	struct xfs_mount	*mp = sc->mp;
	int			error;

	error = xchk_metadata_inode_forks(sc);
	if (error || (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT))
		return error;

	xfs_rmap_ino_bmbt_owner(&oinfo, mp->m_rrmapip->i_ino, XFS_DATA_FORK);
	return xchk_btree(sc, sc->sr.rmap_cur, xchk_rtrmapbt_rec, &oinfo,
			&cr);
}
