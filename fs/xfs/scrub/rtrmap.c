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
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	struct xfs_mount	*mp = sc->mp;
	int			error = 0;

	error = xchk_setup_fs(sc, ip);
	if (error)
		return error;

	/*
	 * rtrmap operations are only performed for files, and therefore always
	 * use deferred operations.  Therefore, we can lock the rt rmap inode
	 * first (as if it were a regular file) and later lock the rtbitmap
	 * file to perform cross-referencing operations.
	 */
	sc->ilock_flags = XFS_ILOCK_EXCL;
	sc->ip = mp->m_rrmapip;
	xfs_ilock(sc->ip, sc->ilock_flags);

	return 0;
}

/* Realtime reverse mapping. */

/* Cross-reference with other metadata. */
STATIC void
xchk_rtrmapbt_xref(
	struct xfs_scrub	*sc,
	struct xfs_rmap_irec	*irec)
{
	xchk_xref_is_used_rt_space(sc, irec->rm_startblock,
			irec->rm_blockcount);
}

/* Scrub a realtime rmapbt record. */
STATIC int
xchk_rtrmapbt_helper(
	struct xchk_btree	*bs,
	union xfs_btree_rec	*rec)
{
	struct xfs_mount	*mp = bs->cur->bc_mp;
	struct xfs_rmap_irec	irec;
	bool			non_inode;
	bool			is_bmbt;
	bool			is_attr;
	int			error;

	error = xfs_rmap_btrec_to_irec(bs->cur, rec, &irec);
	if (!xchk_btree_process_error(bs->sc, bs->cur, 0, &error))
		goto out;

	if (irec.rm_startblock + irec.rm_blockcount <= irec.rm_startblock ||
	    (!xfs_verify_rtbno(mp, irec.rm_startblock) ||
	     !xfs_verify_rtbno(mp, irec.rm_startblock +
				irec.rm_blockcount - 1)))
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	non_inode = XFS_RMAP_NON_INODE_OWNER(irec.rm_owner);
	is_bmbt = irec.rm_flags & XFS_RMAP_BMBT_BLOCK;
	is_attr = irec.rm_flags & XFS_RMAP_ATTR_FORK;

	if (is_bmbt || non_inode || is_attr)
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	if (bs->sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		goto out;

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
	int			error;

	xfs_ilock(mp->m_rbmip, XFS_ILOCK_SHARED | XFS_ILOCK_RTBITMAP);
	xchk_rt_init(sc, &sc->sa);

	xfs_rmap_ino_bmbt_owner(&oinfo, mp->m_rrmapip->i_ino, XFS_DATA_FORK);
	error = xchk_btree(sc, sc->sa.rmap_cur, xchk_rtrmapbt_helper, &oinfo,
			NULL);

	xchk_rt_free(sc, &sc->sa);
	xfs_iunlock(mp->m_rbmip, XFS_ILOCK_SHARED | XFS_ILOCK_RTBITMAP);

	return error;
}
