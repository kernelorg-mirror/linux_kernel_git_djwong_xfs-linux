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
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_inode.h"
#include "xfs_rtalloc.h"
#include "xfs_refcount.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"
#include "scrub/repair.h"

/* Set us up with the realtime metadata locked. */
int
xchk_setup_rtrmapbt(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	int			error;

	if (xchk_could_repair(sc)) {
		error = xrep_setup_rtrmapbt(sc);
		if (error)
			return error;
	}

	error = xchk_setup_fs(sc);
	if (error)
		return error;

	error = xchk_install_inode(sc, mp->m_rrmapip);
	if (error)
		return error;

	xchk_rt_init(sc, &sc->sr);
	return 0;
}

/* Realtime reverse mapping. */

/* Cross-reference a rmap against the refcount btree. */
STATIC void
xchk_rtrmapbt_xref_rtrefc(
	struct xfs_scrub	*sc,
	struct xfs_rmap_irec	*irec)
{
	xfs_rtblock_t		fbno;
	xfs_filblks_t		flen;
	bool			non_inode;
	bool			is_bmbt;
	bool			is_attr;
	bool			is_unwritten;
	int			error;

	if (!sc->sr.refc_cur || xchk_skip_xref(sc->sm))
		return;

	non_inode = XFS_RMAP_NON_INODE_OWNER(irec->rm_owner);
	is_bmbt = irec->rm_flags & XFS_RMAP_BMBT_BLOCK;
	is_attr = irec->rm_flags & XFS_RMAP_ATTR_FORK;
	is_unwritten = irec->rm_flags & XFS_RMAP_UNWRITTEN;

	/* If this is shared, must be a data fork extent. */
	error = xfs_refcount_find_shared(sc->sr.refc_cur, irec->rm_startblock,
			irec->rm_blockcount, &fbno, &flen, false);
	if (!xchk_should_check_xref(sc, &error, &sc->sr.refc_cur))
		return;
	if (flen != 0 && (non_inode || is_attr || is_bmbt || is_unwritten))
		xchk_btree_xref_set_corrupt(sc, sc->sr.refc_cur, 0);
}

/* Cross-reference with other metadata. */
STATIC void
xchk_rtrmapbt_xref(
	struct xfs_scrub	*sc,
	struct xfs_rmap_irec	*irec)
{
	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;

	xchk_xref_is_used_rt_space(sc, irec->rm_startblock,
			irec->rm_blockcount);
	if (irec->rm_owner == XFS_RMAP_OWN_COW)
		xchk_xref_is_cow_staging(sc, irec->rm_startblock,
				irec->rm_blockcount);
	else
		xchk_rtrmapbt_xref_rtrefc(sc, irec);
}

/* Scrub a realtime rmapbt record. */
STATIC int
xchk_rtrmapbt_rec(
	struct xchk_btree		*bs,
	const union xfs_btree_rec	*rec)
{
	struct xfs_mount		*mp = bs->cur->bc_mp;
	struct xfs_rmap_irec		irec;
	bool				non_inode;
	bool				is_bmbt;
	bool				is_attr;
	bool				is_unwritten;
	bool				is_cow;
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
	is_cow = xfs_has_rtreflink(mp) &&
		 irec.rm_owner == XFS_RMAP_OWN_COW;
	is_bmbt = irec.rm_flags & XFS_RMAP_BMBT_BLOCK;
	is_attr = irec.rm_flags & XFS_RMAP_ATTR_FORK;
	is_unwritten = irec.rm_flags & XFS_RMAP_UNWRITTEN;

	if (is_bmbt || (non_inode && !is_cow) || is_attr)
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	if (is_cow && irec.rm_offset != 0)
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	if (is_unwritten && is_cow)
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	if (!non_inode && !xfs_verify_ino(mp, irec.rm_owner))
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	xchk_rtrmapbt_xref(bs->sc, &irec);
out:
	return error;
}

/* Scrub the realtime rmap btree. */
int
xchk_rtrmapbt(
	struct xfs_scrub	*sc)
{
	struct xfs_owner_info	oinfo;
	struct xfs_mount	*mp = sc->mp;

	xfs_rmap_ino_bmbt_owner(&oinfo, mp->m_rrmapip->i_ino, XFS_DATA_FORK);
	return xchk_btree(sc, sc->sr.rmap_cur, xchk_rtrmapbt_rec, &oinfo,
			NULL);
}

/* xref check that the extent has no realtime reverse mapping at all */
void
xchk_xref_has_no_rt_owner(
	struct xfs_scrub	*sc,
	xfs_rtblock_t		bno,
	xfs_filblks_t		len)
{
	bool			has_rmap;
	int			error;

	if (!sc->sr.rmap_cur || xchk_skip_xref(sc->sm))
		return;

	error = xfs_rmap_has_record(sc->sr.rmap_cur, bno, len, &has_rmap);
	if (!xchk_should_check_xref(sc, &error, &sc->sr.rmap_cur))
		return;
	if (has_rmap)
		xchk_btree_xref_set_corrupt(sc, sc->sr.rmap_cur, 0);
}
