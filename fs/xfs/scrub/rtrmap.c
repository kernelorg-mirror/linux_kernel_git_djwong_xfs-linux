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

/* Set us up with the realtime metadata and AG headers locked. */
int
xchk_setup_rtrmapbt(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	struct xfs_mount	*mp = sc->mp;
	int			lockmode;
	int			error = 0;

	if (sc->sm->sm_agno || sc->sm->sm_ino || sc->sm->sm_gen)
		return -EINVAL;

	error = xchk_setup_fs(sc, ip);
	if (error)
		return error;

	lockmode = XFS_ILOCK_EXCL;
	xfs_ilock(mp->m_rrmapip, lockmode);
	xfs_trans_ijoin(sc->tp, mp->m_rrmapip, lockmode);

	lockmode = XFS_ILOCK_EXCL | XFS_ILOCK_RTBITMAP;
	xfs_ilock(mp->m_rbmip, lockmode);
	xfs_trans_ijoin(sc->tp, mp->m_rbmip, lockmode);

	return 0;
}

/* Realtime reverse mapping. */

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
	struct xfs_btree_cur	*cur;
	int			error;

	cur = xfs_rtrmapbt_init_cursor(mp, sc->tp, mp->m_rrmapip);
	xfs_rmap_ino_bmbt_owner(&oinfo, mp->m_rrmapip->i_ino, XFS_DATA_FORK);
	error = xchk_btree(sc, cur, xchk_rtrmapbt_helper, &oinfo, NULL);
	xfs_btree_del_cursor(cur, error);

	return error;
}
