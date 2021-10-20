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
#include "xfs_iwalk.h"
#include "xfs_ialloc.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "xfs_ag.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/repair.h"
#include "scrub/xfarray.h"
#include "scrub/iscan.h"
#include "scrub/nlinks.h"
#include "scrub/trace.h"

/*
 * Live Inode Link Count Checking
 * ==============================
 *
 * Inode link counts are "summary" metadata, in the sense that they are
 * computed as the number of directory entries referencing each file on the
 * filesystem.  Therefore, we compute the correct link counts by creating a
 * shadow link count structure and walking every inode.
 *
 * Because we are scanning a live filesystem, it's possible that another thread
 * will try to update the link counts for an inode that we've already scanned.
 * This will cause our counts to be incorrect.  Therefore, we hook all inode
 * link count updates when the change is made to the incore inode.  By
 * shadowing transaction updates in this manner, live nlink check can ensure by
 * locking the inode and the shadow structure that its own copies are not out
 * of date.
 *
 * Note that we use srcu notifier hooks to minimize the overhead when live
 * nlinks is /not/ running.  Locking order for nlink observations is inode
 * ILOCK -> iscan_lock/xchk_nlink_ctrs lock.
 */

/* Set us up to scrub inode link counts. */
int
xchk_setup_nlinks(
	struct xfs_scrub	*sc)
{
	unsigned int		buf_bytes = sizeof(struct xchk_nlink_ctrs);
	int			error;

	if (xchk_could_repair(sc)) {
		error = xrep_setup_nlinks(sc, &buf_bytes);
		if (error)
			return error;
	}

	sc->buf = kmem_zalloc(buf_bytes, KM_NOFS | KM_MAYFAIL);
	if (!sc->buf)
		return -ENOMEM;

	return xchk_setup_fs(sc);
}

/* Update incore link count information.  Caller must hold the nlinks lock. */
STATIC int
xchk_nlinks_update_incore(
	struct xchk_nlink_ctrs	*xnc,
	struct xfs_inode	*dp,
	bool			parent_to_child,
	xfs_ino_t		ino,
	int			delta)
{
	struct xchk_nlink	nl;
	int			error;

	if (!xnc->nlinks)
		return 0;

	error = xfarray_load_sparse(xnc->nlinks, ino, &nl);
	if (error)
		return error;

	if (parent_to_child)
		nl.parent += delta;
	else
		nl.child += delta;

	error = xfarray_store(xnc->nlinks, ino, &nl);
	if (error == -EFBIG) {
		/*
		 * EFBIG means we tried to store data at too high a byte offset
		 * in the sparse array.  IOWs, we cannot complete the check and
		 * must notify userspace that the check was incomplete.
		 */
		xchk_set_incomplete(xnc->sc);
		error = -ECANCELED;
	}
	return error;
}

/* Read the observed link count for comparison with the actual inode. */
STATIC int
xchk_nlinks_comparison_read(
	struct xchk_nlink_ctrs	*xnc,
	xfs_ino_t		ino,
	struct xchk_nlink	*obs)
{
	struct xchk_nlink	nl;
	int			error;

	error = xfarray_load_sparse(xnc->nlinks, ino, &nl);
	if (error)
		return error;

	nl.flags |= XCHK_NLINK_COMPARE_SCANNED;

	error = xfarray_store(xnc->nlinks, ino, &nl);
	if (error == -EFBIG) {
		/*
		 * EFBIG means we tried to store data at too high a byte offset
		 * in the sparse array.  IOWs, we cannot complete the check and
		 * must notify userspace that the check was incomplete.
		 */
		xchk_set_incomplete(xnc->sc);
		return -ECANCELED;
	}
	if (error)
		return error;

	obs->parent = nl.parent;
	obs->child = nl.child;
	return 0;
}

/*
 * Apply a link count change from the regular filesystem into our shadow link
 * count structure.
 */
STATIC int
xchk_nlinks_live_update(
	struct notifier_block		*nb,
	unsigned long			arg,
	void				*data)
{
	struct xfs_nlink_delta_params	*p = data;
	struct xchk_nlink_ctrs		*xnc;
	bool				parent_to_child;
	int				error;

	parent_to_child = (arg == XFS_PARENT_NLINK_DELTA);
	xnc = container_of(nb, struct xchk_nlink_ctrs, nlink_delta_hook);

	if (!xchk_iscan_want_live_update(&xnc->collect_iscan, p->dp->i_ino))
		return NOTIFY_DONE;

	trace_xchk_nlinks_live_update(xnc->sc->mp, p->dp, parent_to_child,
			p->ino, p->delta);

	mutex_lock(&xnc->lock);
	error = xchk_nlinks_update_incore(xnc, p->dp, parent_to_child, p->ino,
			p->delta);
	mutex_unlock(&xnc->lock);
	if (error)
		xchk_iscan_abort(&xnc->collect_iscan);

	return NOTIFY_DONE;
}

struct xchk_walk_dir {
	struct dir_context	dir_iter;
	struct xchk_nlink_ctrs	*xnc;
	struct xfs_inode	*dp;
};

/* Bump the observed link count for the inode referenced by this entry. */
STATIC int
xchk_nlinks_walk_dir(
	struct dir_context	*dir_iter,
	const char		*name,
	int			namelen,
	loff_t			pos,
	u64			ino,
	unsigned		type)
{
	struct xchk_walk_dir	*xwd;
	struct xchk_nlink_ctrs	*xnc;
	bool			parent_to_child = true;
	int			error = -ECANCELED;

	xwd = container_of(dir_iter, struct xchk_walk_dir, dir_iter);
	xnc = xwd->xnc;

	if (namelen == 0) {
		/* Shouldn't be any zero-length dirents... */
		xchk_set_incomplete(xnc->sc);
		return -ECANCELED;
	} else if (namelen == 1 && name[0] == '.') {
		/*
		 * The dot entry has to point to the directory, and we account
		 * it as a "child" pointing to its parent.
		 */
		if (ino != xwd->dp->i_ino) {
			xchk_set_incomplete(xnc->sc);
			return -ECANCELED;
		}
		parent_to_child = false;
	} else if (namelen == 2 && name[0] == '.' && name[1] == '.') {
		/* dotdot means child pointing to parent */
		parent_to_child = false;
	}

	/* Update the shadow link counts if we haven't already failed. */

	if (xchk_iscan_aborted(&xnc->collect_iscan))
		goto out_incomplete;

	trace_xchk_nlinks_walk_dir(xnc->sc->mp, xwd->dp, parent_to_child, ino,
			name, namelen);
	mutex_lock(&xnc->lock);
	error = xchk_nlinks_update_incore(xnc, xwd->dp, parent_to_child, ino,
			1);
	mutex_unlock(&xnc->lock);
	if (error)
		goto out_abort;

	return 0;

out_abort:
	xchk_iscan_abort(&xnc->collect_iscan);
out_incomplete:
	xchk_set_incomplete(xnc->sc);
	return error;
}

/* Bump the observed link counts of every entry in this directory. */
STATIC int
xchk_nlinks_dir(
	struct xchk_nlink_ctrs	*xnc,
	struct xfs_inode	*dp)
{
	struct xfs_scrub	*sc = xnc->sc;
	struct xchk_walk_dir	xwd = {
		.dir_iter.actor	= xchk_nlinks_walk_dir,
		.dir_iter.pos	= 0,
		.xnc		= xnc,
		.dp		= dp,
	};
	loff_t			oldpos;
	size_t			bufsize;
	unsigned int		lock_mode;
	int			error = 0;

	/* Lock out the VFS from changing this directory while we walk it. */
	xfs_ilock(dp, XFS_IOLOCK_SHARED);

	/*
	 * The dotdot entry of an unlinked directory still points to the last
	 * parent, but the parent no longer links to this directory.  Skip the
	 * directory to avoid overcounting.
	 */
	if (VFS_I(dp)->i_nlink == 0)
		goto out;

	/*
	 * If there are any blocks, read-ahead block 0 as we're almost certain
	 * to have the next operation be a read there.  This is how we
	 * guarantee that the directory's extent map has been loaded, if there
	 * is one.
	 */
	lock_mode = xfs_ilock_data_map_shared(dp);
	if (dp->i_df.if_nextents > 0)
		error = xfs_dir3_data_readahead(dp, 0, 0);
	xfs_iunlock(dp, lock_mode);
	if (error)
		goto out;

	/*
	 * Bump link counts for every dirent we see.  Userspace usually asks
	 * for a 32k buffer, so we will too.
	 */
	bufsize = (size_t)min_t(loff_t, XFS_READDIR_BUFSIZE, dp->i_disk_size);
	do {
		oldpos = xwd.dir_iter.pos;
		error = xfs_readdir(sc->tp, dp, &xwd.dir_iter, bufsize);
	} while (!error && oldpos < xwd.dir_iter.pos);

	xchk_iscan_mark_visited(&xnc->collect_iscan, dp);
out:
	xfs_iunlock(dp, XFS_IOLOCK_SHARED);
	return error;
}

/* If this looks like a valid pointer, count it. */
static inline int
xchk_nlinks_metafile(
	struct xchk_nlink_ctrs	*xnc,
	xfs_ino_t		ino)
{
	if (!xfs_verify_ino(xnc->sc->mp, ino))
		return 0;

	trace_xchk_nlinks_metafile(xnc->sc->mp, ino);
	return xchk_nlinks_update_incore(xnc, NULL, true, ino, 1);
}

/* Bump the link counts of metadata files rooted in the superblock. */
STATIC int
xchk_nlinks_metafiles(
	struct xchk_nlink_ctrs	*xnc)
{
	struct xfs_mount	*mp = xnc->sc->mp;
	int			error = -ECANCELED;


	if (xchk_iscan_aborted(&xnc->collect_iscan))
		goto out_incomplete;

	mutex_lock(&xnc->lock);
	error = xchk_nlinks_metafile(xnc, mp->m_sb.sb_rbmino);
	if (error)
		goto out_abort;

	error = xchk_nlinks_metafile(xnc, mp->m_sb.sb_rsumino);
	if (error)
		goto out_abort;

	error = xchk_nlinks_metafile(xnc, mp->m_sb.sb_uquotino);
	if (error)
		goto out_abort;

	error = xchk_nlinks_metafile(xnc, mp->m_sb.sb_gquotino);
	if (error)
		goto out_abort;

	error = xchk_nlinks_metafile(xnc, mp->m_sb.sb_pquotino);
	if (error)
		goto out_abort;
	mutex_unlock(&xnc->lock);

	return 0;

out_abort:
	mutex_unlock(&xnc->lock);
	xchk_iscan_abort(&xnc->collect_iscan);
out_incomplete:
	xchk_set_incomplete(xnc->sc);
	return error;
}

/* Advance the collection scan cursor for this file. */
static inline int
xchk_nlinks_file(
	struct xchk_nlink_ctrs	*xnc,
	struct xfs_inode	*ip)
{
	xfs_ilock(ip, XFS_IOLOCK_SHARED);
	xchk_iscan_mark_visited(&xnc->collect_iscan, ip);
	xfs_iunlock(ip, XFS_IOLOCK_SHARED);
	return 0;
}

/* Walk all directories and count inode links. */
STATIC int
xchk_nlinks_collect(
	struct xchk_nlink_ctrs	*xnc)
{
	struct xfs_scrub	*sc = xnc->sc;
	int			error;

	/* Count the rt and quota files if they're rooted in the superblock. */
	if (!xfs_has_metadir(sc->mp)) {
		error = xchk_nlinks_metafiles(xnc);
		if (error)
			return error;
	}

	/*
	 * Set up for a potentially lengthy filesystem scan by reducing our
	 * transaction resource usage for the duration.  Specifically:
	 *
	 * Cancel the transaction to release the log grant space while we scan
	 * the filesystem.
	 *
	 * Create a new empty transaction to eliminate the possibility of the
	 * inode scan deadlocking on cyclical metadata.
	 *
	 * We pass the empty transaction to the file scanning function to avoid
	 * repeatedly cycling empty transactions.  This can be done even though
	 * we take the IOLOCK to quiesce the file because empty transactions
	 * do not take sb_internal.
	 */
	xchk_trans_cancel(sc);
	error = xchk_trans_alloc_empty(sc);
	if (error)
		return error;

	while ((error = xchk_iscan_advance(sc, &xnc->collect_iscan)) == 1) {
		struct xfs_inode	*ip;

		error = xchk_iscan_iget(sc, &xnc->collect_iscan, &ip);
		if (error == -EAGAIN)
			continue;
		if (error)
			break;

		if (S_ISDIR(VFS_I(ip)->i_mode))
			error = xchk_nlinks_dir(xnc, ip);
		else
			error = xchk_nlinks_file(xnc, ip);
		xchk_irele(sc, ip);
		if (error)
			break;

		if (xchk_should_terminate(sc, &error))
			break;
	}

	if (error == -ECANCELED)
		xchk_set_incomplete(sc);
	if (error)
		return error;

	/*
	 * Switch out for a real transaction in preparation for building a new
	 * tree.
	 */
	xchk_trans_cancel(sc);
	return xchk_setup_fs(sc);
}

/* Check our link count against an inode. */
STATIC int
xchk_nlinks_compare_inode(
	struct xchk_nlink_ctrs	*xnc,
	struct xfs_inode	*ip)
{
	struct xchk_nlink	obs;
	struct xfs_scrub	*sc = xnc->sc;
	uint64_t		total_links;
	unsigned int		actual_nlink;
	int			error;

	xfs_ilock(ip, XFS_ILOCK_SHARED);
	mutex_lock(&xnc->lock);

	if (xchk_iscan_aborted(&xnc->collect_iscan)) {
		xchk_set_incomplete(xnc->sc);
		error = -ECANCELED;
		goto out_scanlock;
	}

	error = xchk_nlinks_comparison_read(xnc, ip->i_ino, &obs);
	if (error)
		goto out_scanlock;
	total_links = xchk_nlink_total(&obs);
	actual_nlink = VFS_I(ip)->i_nlink;

	trace_xchk_nlinks_compare_inode(sc->mp, ip, &obs);

	/* We found more than the maxiumum possible link count. */
	if (total_links > U32_MAX)
		xchk_ino_set_corrupt(sc, ip->i_ino);

	/* Link counts should match. */
	if (total_links != actual_nlink)
		xchk_ino_set_corrupt(sc, ip->i_ino);

	/*
	 * Directories with nonzero link count must have at least one child
	 * (dot entry).  The collection phase ignores directories with zero
	 * link count, so we ignore them here too.
	 */
	if (S_ISDIR(VFS_I(ip)->i_mode) && actual_nlink > 0 && obs.child < 1)
		xchk_ino_set_corrupt(sc, ip->i_ino);

	/* Non-directories should not have children */
	if (!S_ISDIR(VFS_I(ip)->i_mode) && obs.child != 0)
		xchk_ino_set_corrupt(sc, ip->i_ino);

	if (ip == sc->mp->m_metadirip || ip == sc->mp->m_rootip) {
		/* Nothing should point to the directory tree roots. */
		if (obs.parent != 0)
			xchk_ino_set_corrupt(sc, ip->i_ino);

		/*
		 * Directory tree roots should have at least two "child"
		 * references to cover dot and dotdot.
		 */
		if (obs.child < 2)
			xchk_ino_set_corrupt(sc, ip->i_ino);
	} else if (obs.parent == 0) {
		/* Non-root linked files should have a parent. */
		if (actual_nlink != 0)
			xchk_ino_set_corrupt(sc, ip->i_ino);
	}

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		error = -EFSCORRUPTED;

out_scanlock:
	mutex_unlock(&xnc->lock);
	xfs_iunlock(ip, XFS_ILOCK_SHARED);
	return error;
}

/*
 * Check our link count against an inode that wasn't checked previously.  This
 * is intended to catch directories with dangling links, though we could be
 * racing with inode allocation in other threads.
 */
STATIC int
xchk_nlinks_compare_inum(
	struct xchk_nlink_ctrs	*xnc,
	xfs_ino_t		ino)
{
	struct xchk_nlink	obs;
	struct xfs_mount	*mp = xnc->sc->mp;
	struct xfs_trans	*tp = xnc->sc->tp;
	struct xfs_buf		*agi_bp;
	struct xfs_inode	*ip;
	int			error;

	/*
	 * Lock the AGI to the transaction just in case the lookup fails and we
	 * need something to prevent inode allocation while we reconfirm the
	 * observed nlink value.
	 */
	error = xfs_ialloc_read_agi(mp, tp, XFS_INO_TO_AGNO(mp, ino), &agi_bp);
	if (error)
		return error;

	error = xfs_iget(mp, tp, ino, XFS_IGET_UNTRUSTED, 0, &ip);
	if (error == 0) {
		/* Actually got an inode, so use the inode compare. */
		xfs_trans_brelse(tp, agi_bp);
		error = xchk_nlinks_compare_inode(xnc, ip);
		xchk_irele(xnc->sc, ip);
		return error;
	}
	if (error == -ENOENT || error == -EINVAL) {
		/* No inode was found; check for zero link count below. */
		error = 0;
	}
	if (error)
		goto out_agi;

	if (xchk_iscan_aborted(&xnc->collect_iscan)) {
		xchk_set_incomplete(xnc->sc);
		error = -ECANCELED;
		goto out_agi;
	}

	mutex_lock(&xnc->lock);
	error = xchk_nlinks_comparison_read(xnc, ino, &obs);
	if (error)
		goto out_scanlock;

	trace_xchk_nlinks_check_zero(mp, ino, &obs);

	/*
	 * If we can't grab the inode, the link count had better be zero.  We
	 * still hold the AGI to prevent inode allocation/freeing.
	 */
	if (xchk_nlink_total(&obs) != 0) {
		xchk_ino_set_corrupt(xnc->sc, ino);
		error = -ECANCELED;
	}

out_scanlock:
	mutex_unlock(&xnc->lock);
out_agi:
	xfs_trans_brelse(tp, agi_bp);
	return error;
}

/* Compare the link counts we observed against the live information. */
STATIC int
xchk_nlinks_compare(
	struct xchk_nlink_ctrs	*xnc)
{
	struct xchk_nlink	nl;
	struct xfs_scrub	*sc = xnc->sc;
	uint64_t		nr = 0;
	int			error;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return 0;

	/*
	 * Create a new empty transaction so that we can advance the iscan
	 * cursor without deadlocking if the inobt has a cycle and push on the
	 * inactivation workqueue.
	 */
	xchk_trans_cancel(sc);
	error = xchk_trans_alloc_empty(sc);
	if (error)
		return error;

	/*
	 * Use the inobt to walk all allocated inodes to compare the link
	 * counts.  If we can't grab the inode, we'll try again in the second
	 * step.
	 */
	xchk_iscan_start(&xnc->compare_iscan);
	while ((error = xchk_iscan_advance(sc, &xnc->compare_iscan)) == 1) {
		struct xfs_inode	*ip;

		error = xchk_iscan_iget(sc, &xnc->compare_iscan, &ip);
		if (error == -ECANCELED)
			continue;
		if (error)
			break;

		error = xchk_nlinks_compare_inode(xnc, ip);
		xchk_iscan_mark_visited(&xnc->compare_iscan, ip);
		xchk_irele(sc, ip);
		if (error)
			break;

		if (xchk_should_terminate(sc, &error))
			break;
	}
	xchk_iscan_finish(&xnc->compare_iscan);
	if (error)
		return error;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return 0;

	/*
	 * Walk all the non-null nlink observations that weren't checked in the
	 * previous step.
	 */
	mutex_lock(&xnc->lock);
	while ((error = xfarray_iter(xnc->nlinks, &nr, &nl)) == 1) {
		xfs_ino_t	ino = nr - 1;

		if (nl.flags & XCHK_NLINK_COMPARE_SCANNED)
			continue;

		mutex_unlock(&xnc->lock);

		error = xchk_nlinks_compare_inum(xnc, ino);
		if (error)
			return error;

		if (xchk_should_terminate(xnc->sc, &error))
			return error;

		mutex_lock(&xnc->lock);
	}
	mutex_unlock(&xnc->lock);

	return error;
}

/* Tear down everything associated with a nlinks check. */
static void
xchk_nlinks_teardown_scan(
	struct xchk_nlink_ctrs	*xnc)
{
	/* Discourage any hook functions that might be running. */
	xchk_iscan_abort(&xnc->collect_iscan);

	xfs_hook_del(&xnc->sc->mp->m_nlink_delta_hooks, &xnc->nlink_delta_hook);

	xfarray_destroy(xnc->nlinks);
	xnc->nlinks = NULL;

	xchk_iscan_finish(&xnc->collect_iscan);
	mutex_destroy(&xnc->lock);
	xnc->sc = NULL;
}

/*
 * Scan all inodes in the entire filesystem to generate link count data.  If
 * the scan is successful, the counts will be left alive for a repair.  If any
 * error occurs, we'll tear everything down.
 */
STATIC int
xchk_nlinks_setup_scan(
	struct xfs_scrub	*sc,
	struct xchk_nlink_ctrs	*xnc)
{
	int			error;

	ASSERT(xnc->sc == NULL);
	xnc->sc = sc;

	mutex_init(&xnc->lock);
	xnc->collect_iscan.iget_tries = 20;
	xnc->collect_iscan.iget_retry_delay = HZ / 10;
	xchk_iscan_start(&xnc->collect_iscan);

	error = xfarray_create(sc->mp, "file link counts",
			sizeof(struct xchk_nlink), &xnc->nlinks);
	if (error)
		goto out_teardown;

	/*
	 * Hook into the bumplink/droplink code.  The hook only triggers for
	 * inodes that were already scanned, and the scanner thread takes each
	 * inode's ILOCK, which means that any in-progress inode updates will
	 * finish before we can scan the inode.
	 */
	error = xfs_hook_add(&sc->mp->m_nlink_delta_hooks,
			&xnc->nlink_delta_hook, xchk_nlinks_live_update);
	if (error)
		goto out_teardown;

	/* Use deferred cleanup to pass the inode link count data to repair. */
	sc->buf_cleanup = (void (*)(void *))xchk_nlinks_teardown_scan;
	return 0;

out_teardown:
	xchk_nlinks_teardown_scan(xnc);
	return error;
}

/* Scrub the link count of all inodes on the filesystem. */
int
xchk_nlinks(
	struct xfs_scrub	*sc)
{
	struct xchk_nlink_ctrs	*xnc = sc->buf;
	int			error = 0;

	/* Set ourselves up to check link counts on the live filesystem. */
	error = xchk_nlinks_setup_scan(sc, xnc);
	if (error)
		return error;

	/* Walk all inodes, picking up link count information. */
	error = xchk_nlinks_collect(xnc);
	if (!xchk_xref_process_error(sc, 0, 0, &error))
		return error;

	/* Compare link counts. */
	error = xchk_nlinks_compare(xnc);
	if (!xchk_xref_process_error(sc, 0, 0, &error))
		return error;

	return 0;
}
