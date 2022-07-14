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
#include "scrub/xfarray.h"
#include "scrub/iscan.h"
#include "scrub/nlinks.h"
#include "scrub/trace.h"
#include "scrub/tempfile.h"

/*
 * Live Inode Link Count Repair
 * ============================
 *
 * Use the live inode link count information that we collected to replace the
 * nlink values of the incore inodes.  A scrub->repair cycle should have left
 * the live data and hooks active, so this is safe so long as we make sure the
 * inode is locked.
 */

/*
 * Correct the link count of the given inode.  Because we have to grab locks
 * and resources in a certain order, it's possible that this will be a no-op.
 */
STATIC int
xrep_nlinks_repair_inode(
	struct xchk_nlink_ctrs	*xnc)
{
	struct xchk_nlink	obs;
	struct xfs_scrub	*sc = xnc->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_inode	*ip = sc->ip;
	uint64_t		total_links;
	unsigned int		actual_nlink;
	int			error;

	/*
	 * Ignore temporary files being used to stage repairs, since we assume
	 * they're correct for non-directories, and the directory repair code
	 * doesn't bump the link counts for the children.
	 */
	if (xrep_is_tempfile(ip))
		return 0;

	xfs_ilock(ip, XFS_IOLOCK_EXCL);

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_link, 0, 0, 0, &sc->tp);
	if (error)
		goto out_iolock;

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(sc->tp, ip, 0);

	mutex_lock(&xnc->lock);

	if (xchk_iscan_aborted(&xnc->collect_iscan)) {
		error = -ECANCELED;
		goto out_scanlock;
	}

	error = xfarray_load_sparse(xnc->nlinks, ip->i_ino, &obs);
	if (error)
		goto out_scanlock;

	total_links = xchk_nlink_total(&obs);
	actual_nlink = VFS_I(ip)->i_nlink;

	/*
	 * Cannot set more than the maxiumum possible link count.  We won't
	 * touch this file, but we don't want to abort the entire operation,
	 * so set an explicit error value to avoid static checker warnings and
	 * return early.
	 */
	if (total_links > U32_MAX) {
		trace_xrep_nlinks_unfixable_inode(mp, ip, &obs);
		goto out_nextfile;
	}

	/*
	 * Linked directories should have at least one "child" (the dot entry)
	 * pointing up to them.
	 */
	if (S_ISDIR(VFS_I(ip)->i_mode) && actual_nlink > 0 &&
					  obs.children == 0) {
		trace_xrep_nlinks_unfixable_inode(mp, ip, &obs);
		goto out_nextfile;
	}

	/* Non-directories cannot have directories pointing up to them. */
	if (!S_ISDIR(VFS_I(ip)->i_mode) && obs.children != 0) {
		trace_xrep_nlinks_unfixable_inode(mp, ip, &obs);
		goto out_nextfile;
	}

	/*
	 * We did not find any links to this inode.  If the inode agrees, we
	 * have nothing further to do.  If not, the inode has a nonzero link
	 * count and we don't have anywhere to graft the child onto.  Dropping
	 * a live inode's link count to zero can cause unexpected shutdowns in
	 * inactivation, so leave it alone.
	 */
	if (total_links == 0) {
		if (actual_nlink != 0)
			trace_xrep_nlinks_unfixable_inode(mp, ip, &obs);
		goto out_nextfile;
	}

	/* Perfect match means we're done with this file. */
	if (total_links == actual_nlink)
		goto out_nextfile;

	mutex_unlock(&xnc->lock);

	/* Commit the new link count. */
	trace_xrep_nlinks_update_inode(mp, ip, &obs);

	set_nlink(VFS_I(ip), total_links);
	xfs_trans_log_inode(sc->tp, ip, XFS_ILOG_CORE);
	error = xfs_trans_commit(sc->tp);
	sc->tp = NULL;
	if (error)
		goto out_ilock;

	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	xfs_iunlock(ip, XFS_IOLOCK_EXCL);
	return 0;

out_nextfile:
	error = 0;
out_scanlock:
	mutex_unlock(&xnc->lock);
	xchk_trans_cancel(sc);
out_ilock:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
out_iolock:
	xfs_iunlock(ip, XFS_IOLOCK_EXCL);
	return error;
}

/*
 * Try to visit every inode in the filesystem for repairs.  Move on if we can't
 * grab an inode, since we're still making forward progress.
 */
static int
xrep_nlinks_iter(
	struct xchk_nlink_ctrs	*xnc,
	struct xfs_inode	**ipp)
{
	int			error;

	do {
		error = xchk_iscan_iter(xnc->sc, &xnc->compare_iscan, ipp);
	} while (error == -EBUSY);

	return error;
}

/* Commit the new inode link counters. */
int
xrep_nlinks(
	struct xfs_scrub	*sc)
{
	struct xchk_nlink_ctrs	*xnc = sc->buf;
	int			error;

	/*
	 * We need ftype for an accurate count of the number of child
	 * subdirectory links.  Child subdirectories with a back link (dotdot
	 * entry) but no forward link are unfixable, so we cannot repair the
	 * link count of the parent directory based on the back link count
	 * alone.  Filesystems without ftype support are rare (old V4) so we
	 * just skip out here.
	 */
	if (!xfs_has_ftype(sc->mp))
		return -EOPNOTSUPP;

	/*
	 * Use the inobt to walk all allocated inodes to compare and fix the
	 * link counts.  Retry iget every tenth of a second for up to 30
	 * seconds -- even if repair misses a few inodes, we still try to fix
	 * as many of them as we can.
	 */
	xchk_iscan_start(&xnc->compare_iscan, 30000, 100);
	ASSERT(sc->ip == NULL);

	while ((error = xrep_nlinks_iter(xnc, &sc->ip)) == 1) {
		/*
		 * Commit the scrub transaction so that we can create repair
		 * transactions with the correct reservations.
		 */
		xchk_trans_cancel(sc);

		error = xrep_nlinks_repair_inode(xnc);
		xchk_iscan_mark_visited(&xnc->compare_iscan, sc->ip);
		xchk_irele(sc, sc->ip);
		sc->ip = NULL;
		if (error)
			break;

		if (xchk_should_terminate(sc, &error))
			break;

		/*
		 * Create a new empty transaction so that we can advance the
		 * iscan cursor without deadlocking if the inobt has a cycle.
		 * We can only push the inactivation workqueues with an empty
		 * transaction.
		 */
		error = xchk_trans_alloc_empty(sc);
		if (error)
			break;
	}
	xchk_iscan_finish(&xnc->compare_iscan);

	return error;
}
