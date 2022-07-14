// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_btree.h"
#include "xfs_rmap.h"
#include "xfs_refcount.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "xfs_ag.h"

/*
 * Set us up to scrub reverse mapping btrees.
 */
int
xchk_setup_ag_rmapbt(
	struct xfs_scrub	*sc)
{
	if (xchk_need_fshook_drain(sc))
		xchk_fshooks_enable(sc, XCHK_FSHOOKS_DRAIN);

	return xchk_setup_ag_btree(sc, false);
}

/* Reverse-mapping scrubber. */

struct xchk_rmap {
	/*
	 * The furthest-reaching of the rmapbt records that we've already
	 * processed.  This enables us to detect overlapping records for space
	 * allocations that cannot be shared.
	 */
	struct xfs_rmap_irec	overlap_rec;
};

/* Cross-reference a rmap against the refcount btree. */
STATIC void
xchk_rmapbt_xref_refc(
	struct xfs_scrub	*sc,
	struct xfs_rmap_irec	*irec)
{
	xfs_agblock_t		fbno;
	xfs_extlen_t		flen;
	bool			non_inode;
	bool			is_bmbt;
	bool			is_attr;
	bool			is_unwritten;
	int			error;

	if (!sc->sa.refc_cur || xchk_skip_xref(sc->sm))
		return;

	non_inode = XFS_RMAP_NON_INODE_OWNER(irec->rm_owner);
	is_bmbt = irec->rm_flags & XFS_RMAP_BMBT_BLOCK;
	is_attr = irec->rm_flags & XFS_RMAP_ATTR_FORK;
	is_unwritten = irec->rm_flags & XFS_RMAP_UNWRITTEN;

	/* If this is shared, must be a data fork extent. */
	error = xfs_refcount_find_shared(sc->sa.refc_cur, irec->rm_startblock,
			irec->rm_blockcount, &fbno, &flen, false);
	if (!xchk_should_check_xref(sc, &error, &sc->sa.refc_cur))
		return;
	if (flen != 0 && (non_inode || is_attr || is_bmbt || is_unwritten))
		xchk_btree_xref_set_corrupt(sc, sc->sa.refc_cur, 0);
}

/* Cross-reference with the other btrees. */
STATIC void
xchk_rmapbt_xref(
	struct xfs_scrub	*sc,
	struct xfs_rmap_irec	*irec)
{
	xfs_agblock_t		agbno = irec->rm_startblock;
	xfs_extlen_t		len = irec->rm_blockcount;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;

	xchk_xref_is_used_space(sc, agbno, len);
	if (irec->rm_owner == XFS_RMAP_OWN_INODES)
		xchk_xref_is_inode_chunk(sc, agbno, len);
	else
		xchk_xref_is_not_inode_chunk(sc, agbno, len);
	if (irec->rm_owner == XFS_RMAP_OWN_COW)
		xchk_xref_is_cow_staging(sc, irec->rm_startblock,
				irec->rm_blockcount);
	else
		xchk_rmapbt_xref_refc(sc, irec);
}

static inline bool
xchk_rmapbt_is_shareable(
	struct xfs_scrub		*sc,
	const struct xfs_rmap_irec	*irec)
{
	if (!xfs_has_reflink(sc->mp))
		return false;
	if (XFS_RMAP_NON_INODE_OWNER(irec->rm_owner))
		return false;
	if (irec->rm_flags & (XFS_RMAP_BMBT_BLOCK | XFS_RMAP_ATTR_FORK |
			      XFS_RMAP_UNWRITTEN))
		return false;
	return true;
}

/* Flag failures for records that overlap but cannot. */
STATIC void
xchk_rmapbt_check_overlapping(
	struct xchk_btree		*bs,
	struct xchk_rmap		*cr,
	const struct xfs_rmap_irec	*irec)
{
	xfs_agblock_t			pnext, inext;

	if (bs->sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;

	/* No previous record? */
	if (cr->overlap_rec.rm_blockcount == 0)
		goto set_prev;

	/* Do overlap_rec and irec overlap? */
	pnext = cr->overlap_rec.rm_startblock + cr->overlap_rec.rm_blockcount;
	if (pnext <= irec->rm_startblock)
		goto set_prev;

	/* Overlap is only allowed if both records are data fork mappings. */
	if (!xchk_rmapbt_is_shareable(bs->sc, &cr->overlap_rec) ||
	    !xchk_rmapbt_is_shareable(bs->sc, irec))
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	/* Save whichever rmap record extends furthest. */
	inext = irec->rm_startblock + irec->rm_blockcount;
	if (pnext > inext)
		return;

set_prev:
	memcpy(&cr->overlap_rec, irec, sizeof(struct xfs_rmap_irec));
}

/* Scrub an rmapbt record. */
STATIC int
xchk_rmapbt_rec(
	struct xchk_btree	*bs,
	const union xfs_btree_rec *rec)
{
	struct xfs_mount	*mp = bs->cur->bc_mp;
	struct xchk_rmap	*cr = bs->private;
	struct xfs_rmap_irec	irec;
	struct xfs_perag	*pag = bs->cur->bc_ag.pag;
	bool			non_inode;
	bool			is_unwritten;
	bool			is_bmbt;
	bool			is_attr;
	int			error;

	error = xfs_rmap_btrec_to_irec(rec, &irec);
	if (!xchk_btree_process_error(bs->sc, bs->cur, 0, &error))
		goto out;

	/* Check extent. */
	if (irec.rm_startblock + irec.rm_blockcount <= irec.rm_startblock)
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	if (irec.rm_owner == XFS_RMAP_OWN_FS) {
		/*
		 * xfs_verify_agbno returns false for static fs metadata.
		 * Since that only exists at the start of the AG, validate
		 * that by hand.
		 */
		if (irec.rm_startblock != 0 ||
		    irec.rm_blockcount != XFS_AGFL_BLOCK(mp) + 1)
			xchk_btree_set_corrupt(bs->sc, bs->cur, 0);
	} else {
		/*
		 * Otherwise we must point somewhere past the static metadata
		 * but before the end of the FS.  Run the regular check.
		 */
		if (!xfs_verify_agbno(pag, irec.rm_startblock) ||
		    !xfs_verify_agbno(pag, irec.rm_startblock +
				irec.rm_blockcount - 1))
			xchk_btree_set_corrupt(bs->sc, bs->cur, 0);
	}

	/* Check flags. */
	non_inode = XFS_RMAP_NON_INODE_OWNER(irec.rm_owner);
	is_bmbt = irec.rm_flags & XFS_RMAP_BMBT_BLOCK;
	is_attr = irec.rm_flags & XFS_RMAP_ATTR_FORK;
	is_unwritten = irec.rm_flags & XFS_RMAP_UNWRITTEN;

	if (is_bmbt && irec.rm_offset != 0)
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	if (non_inode && irec.rm_offset != 0)
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	if (is_unwritten && (is_bmbt || non_inode || is_attr))
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	if (non_inode && (is_bmbt || is_unwritten || is_attr))
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	if (!non_inode) {
		if (!xfs_verify_ino(mp, irec.rm_owner))
			xchk_btree_set_corrupt(bs->sc, bs->cur, 0);
	} else {
		/* Non-inode owner within the magic values? */
		if (irec.rm_owner <= XFS_RMAP_OWN_MIN ||
		    irec.rm_owner > XFS_RMAP_OWN_FS)
			xchk_btree_set_corrupt(bs->sc, bs->cur, 0);
	}

	xchk_rmapbt_check_overlapping(bs, cr, &irec);
	xchk_rmapbt_xref(bs->sc, &irec);
out:
	return error;
}

/* Scrub the rmap btree for some AG. */
int
xchk_rmapbt(
	struct xfs_scrub	*sc)
{
	struct xchk_rmap	*cr;
	int			error;

	cr = kzalloc(sizeof(struct xchk_rmap), XCHK_GFP_FLAGS);
	if (!cr)
		return -ENOMEM;

	error = xchk_btree(sc, sc->sa.rmap_cur, xchk_rmapbt_rec,
			&XFS_RMAP_OINFO_AG, cr);
	kfree(cr);
	return error;
}

/* xref check that the extent is owned only by a given owner */
void
xchk_xref_is_only_owned_by(
	struct xfs_scrub		*sc,
	xfs_agblock_t			bno,
	xfs_extlen_t			len,
	const struct xfs_owner_info	*oinfo)
{
	struct xfs_rmap_matches		res;
	int				error;

	if (!sc->sa.rmap_cur || xchk_skip_xref(sc->sm))
		return;

	error = xfs_rmap_count_owners(sc->sa.rmap_cur, bno, len, oinfo, &res);
	if (!xchk_should_check_xref(sc, &error, &sc->sa.rmap_cur))
		return;
	if (res.matches != 1)
		xchk_btree_xref_set_corrupt(sc, sc->sa.rmap_cur, 0);
	if (res.badno_matches)
		xchk_btree_xref_set_corrupt(sc, sc->sa.rmap_cur, 0);
	if (res.nono_matches)
		xchk_btree_xref_set_corrupt(sc, sc->sa.rmap_cur, 0);
}

/* xref check that the extent is not owned by a given owner */
void
xchk_xref_is_not_owned_by(
	struct xfs_scrub		*sc,
	xfs_agblock_t			bno,
	xfs_extlen_t			len,
	const struct xfs_owner_info	*oinfo)
{
	struct xfs_rmap_matches		res;
	int				error;

	if (!sc->sa.rmap_cur || xchk_skip_xref(sc->sm))
		return;

	error = xfs_rmap_count_owners(sc->sa.rmap_cur, bno, len, oinfo, &res);
	if (!xchk_should_check_xref(sc, &error, &sc->sa.rmap_cur))
		return;
	if (res.matches != 0)
		xchk_btree_xref_set_corrupt(sc, sc->sa.rmap_cur, 0);
	if (res.badno_matches)
		xchk_btree_xref_set_corrupt(sc, sc->sa.rmap_cur, 0);
}

/* xref check that the extent has no reverse mapping at all */
void
xchk_xref_has_no_owner(
	struct xfs_scrub	*sc,
	xfs_agblock_t		bno,
	xfs_extlen_t		len)
{
	enum xfs_btree_keyfill	keyfill;
	int			error;

	if (!sc->sa.rmap_cur || xchk_skip_xref(sc->sm))
		return;

	error = xfs_rmap_scan_keyfill(sc->sa.rmap_cur, bno, len, &keyfill);
	if (!xchk_should_check_xref(sc, &error, &sc->sa.rmap_cur))
		return;
	if (keyfill != XFS_BTREE_KEYFILL_EMPTY)
		xchk_btree_xref_set_corrupt(sc, sc->sa.rmap_cur, 0);
}
