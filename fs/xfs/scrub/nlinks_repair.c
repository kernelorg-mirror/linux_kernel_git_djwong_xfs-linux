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
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_bmap_util.h"
#include "xfs_iwalk.h"
#include "xfs_ialloc.h"
#include "xfs_sb.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/repair.h"
#include "scrub/array.h"
#include "scrub/iscan.h"
#include "scrub/nlinks.h"
#include "scrub/trace.h"

/*
 * Live Inode Link Count Repair
 * ============================
 *
 * Use the live inode link count information that we collected to replace the
 * nlink values of the incore inodes.  A scrub->repair cycle should have left
 * the live data and hooks active, so this is safe so long as we make sure the
 * inode is locked.
 */

/* Update incore link count information.  Caller must hold the xnc lock. */
static int
xrep_nlinks_ino_set(
	struct xchk_nlinks	*xnc,
	xfs_ino_t		ino,
	xfs_nlink_t		nlinks)
{
	int			error;

	trace_xrep_nlinks_ino_set(xnc->sc->mp, ino, nlinks);

	if (!xnc->nlinks)
		return 0;

	error = xfbma_set(xnc->nlinks, ino, &nlinks);
	if (error == -EFBIG) {
		/*
		 * EFBIG means we tried to store data at too high a byte offset
		 * in the sparse array.  This should be impossible since we
		 * presumably already stored an nlink count, but we still need
		 * to fail gracefully.
		 */
		return -ECANCELED;
	}

	return error;
}

/* Commit new counters to an inode. */
static int
xrep_nlinks_commit_inode(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp_unused,
	xfs_ino_t		ino,
	void			*data)
{
	struct xchk_nlinks	*xnc = data;
	struct xfs_inode	*ip = NULL;
	struct xfs_trans	*tp;
	xfs_nlink_t		live_nlink;
	int			error;

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_ichange, 0, 0, 0, &tp);
	if (error)
		return error;

	error = xfs_iget(mp, tp, ino, XFS_IGET_UNTRUSTED, XFS_ILOCK_EXCL, &ip);
	if (error == -ENOENT || error == -EINVAL) {
		/* Inode wasn't found, so we'll check for zero nlink. */
		error = 0;
	}
	if (error)
		goto out_cancel;

	xchk_iscan_lock(&xnc->iscan);
	if (xnc->hook_dead) {
		error = -ECANCELED;
		goto out_unlock;
	}
	error = xchk_nlinks_get_shadow_count(xnc, ino, &live_nlink);
	if (error)
		goto out_unlock;

	if (ip == NULL) {
		/*
		 * We couldn't get the inode, so the link count had better be
		 * zero!  This means we cannot fix the filesystem.
		 */
		if (live_nlink != 0) {
			trace_xrep_nlinks_unfixable_inode(mp, ino, 0,
					live_nlink);
			error = -EFSCORRUPTED;
		}
		goto out_unlock;
	}

	/*
	 * We can't have directories with a live nlink count of 1.  We grabbed
	 * the directory inode, which means that the ondisk inode has a nonzero
	 * nlink.  Set the link counts to two.
	 */
	if (S_ISDIR(VFS_I(ip)->i_mode) && live_nlink == 1) {
		live_nlink = 2;
		error = xrep_nlinks_ino_set(xnc, ip->i_ino, live_nlink);
		if (error)
			goto out_unlock;
	}

	if (live_nlink == VFS_I(ip)->i_nlink)
		goto out_unlock;

	/* Commit the new link count, if necessary. */
	xchk_iscan_unlock(&xnc->iscan);

	trace_xrep_nlinks_commit_inode(mp, ino, VFS_I(ip)->i_nlink,
			live_nlink);

	xfs_trans_ijoin(tp, ip, XFS_ILOCK_EXCL);
	set_nlink(VFS_I(ip), live_nlink);
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	error = xfs_trans_commit(tp);
	xchk_irele(xnc->sc, ip);
	return error;

out_unlock:
	xchk_iscan_unlock(&xnc->iscan);
	if (ip) {
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
		xchk_irele(xnc->sc, ip);
	}
out_cancel:
	xfs_trans_cancel(tp);
	return error;
}

/* Commit the new inode link counters. */
int
xrep_nlinks(
	struct xfs_scrub	*sc)
{
	struct xchk_nlinks	*xnc = sc->buf;
	uint64_t		nr = 0;
	xfs_nlink_t		nlink;
	int			error;

	/* Commit the scrub transaction so that we can use multithreaded iwalk. */
	error = xfs_trans_commit(sc->tp);
	sc->tp = NULL;
	if (error)
		return error;

	/* Update the link count of every inode that the inobt knows about. */
	error = xfs_iwalk_threaded(sc->mp, 0, XFS_IWALK_METADIR,
			xrep_nlinks_commit_inode, 0, false, xnc);
	if (error)
		return error;

	/*
	 * Make a second pass to find inodes for which we have positive link
	 * count but don't seem to exist on disk.  We cannot resuscitate dead
	 * inodes, but we can at least signal failure.
	 */
	xchk_iscan_lock(&xnc->iscan);
	while (!(error = xfbma_iter_get(xnc->nlinks, &nr, &nlink))) {
		xchk_iscan_unlock(&xnc->iscan);

		if (xchk_should_terminate(xnc->sc, &error))
			return error;

		error = xrep_nlinks_commit_inode(sc->mp, NULL, nr - 1, xnc);
		if (error)
			return error;

		xchk_iscan_lock(&xnc->iscan);
	}
	xchk_iscan_unlock(&xnc->iscan);

	/* ENODATA means we hit the end of the array. */
	if (error == -ENODATA)
		return 0;

	return error;
}
