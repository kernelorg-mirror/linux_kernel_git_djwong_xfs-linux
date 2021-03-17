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

/* Commit new counters to an inode. */
static int
xrep_nlinks_commit_inode(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	xfs_ino_t		ino,
	void			*data)
{
	struct xchk_nlinks	*xnc = data;
	struct xfs_inode	*ip = NULL;
	xfs_nlink_t		live_nlink;
	int			error;

	error = xfs_iget(mp, tp, ino, XFS_IGET_UNTRUSTED | XFS_IGET_UNLINKED,
			XFS_ILOCK_EXCL, &ip);
	if (error == -ENOENT || error == -EINVAL) {
		/* Inode wasn't found, so we'll check for zero nlink. */
		error = 0;
	}
	if (error)
		return error;

	mutex_lock(&xnc->lock);
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

	/* Commit the new link count, if necessary. */
	if (live_nlink != VFS_I(ip)->i_nlink) {
		trace_xrep_nlinks_commit_inode(mp, ino, VFS_I(ip)->i_nlink,
				live_nlink);

		xfs_trans_ijoin(tp, ip, 0);
		set_nlink(VFS_I(ip), live_nlink);
		xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
		error = xrep_roll_trans(xnc->sc);
	}

out_unlock:
	if (error)
		xnc->hook_dead = true;
	mutex_unlock(&xnc->lock);
	if (ip) {
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
		xchk_irele(xnc->sc, ip);
	}
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

	/* Update the link count of every inode that the inobt knows about. */
	error = xfs_iwalk(sc->mp, sc->tp, 0, XFS_IWALK_METADIR,
			xrep_nlinks_commit_inode, 0, xnc);
	if (error)
		return error;

	/*
	 * Make a second pass to find inodes for which we have positive link
	 * count but don't seem to exist on disk.  We cannot resuscitate dead
	 * inodes, but we can at least signal failure.
	 */
	mutex_lock(&xnc->lock);
	while (!(error = xfbma_iter_get(xnc->nlinks, &nr, &nlink))) {
		mutex_unlock(&xnc->lock);

		if (xchk_should_terminate(xnc->sc, &error))
			return error;

		error = xrep_nlinks_commit_inode(sc->mp, sc->tp, nr - 1, xnc);
		if (error)
			return error;

		mutex_lock(&xnc->lock);
	}
	mutex_unlock(&xnc->lock);

	/* ENODATA means we hit the end of the array. */
	if (error == -ENODATA)
		return 0;

	return error;
}
