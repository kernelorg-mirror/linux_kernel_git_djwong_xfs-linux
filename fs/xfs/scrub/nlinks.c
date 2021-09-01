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
#include "scrub/array.h"
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
 * nlinks is /not/ running.
 */

/* Set us up to scrub inode link counts. */
int
xchk_setup_nlinks(
	struct xfs_scrub	*sc)
{
	sc->buf = kmem_zalloc(sizeof(struct xchk_nlinks), KM_NOFS | KM_MAYFAIL);
	if (!sc->buf)
		return -ENOMEM;

	return xchk_setup_fs(sc);
}

/* Retrieve the shadow link count for the given inode. */
int
xchk_nlinks_get_shadow_count(
	struct xchk_nlinks	*xnc,
	xfs_ino_t		ino,
	xfs_nlink_t		*nlinks)
{
	int			error;

	error = xfbma_get(xnc->nlinks, ino, nlinks);
	if (error == -ENODATA) {
		/*
		 * ENODATA means we tried to read beyond the end of the sparse
		 * array.  This isn't a big deal, just zero the incore record
		 * and return that.
		 */
		*nlinks = 0;
		return 0;
	}
	return error;
}

/* Update incore link count information.  Caller must hold the iscan lock. */
static int
xchk_nlinks_update_incore(
	struct xchk_nlinks	*xnc,
	struct xfs_inode	*dp,
	xfs_ino_t		ino,
	int			delta)
{
	xfs_nlink_t		nlinks;
	int			error;

	trace_xchk_nlinks_update_incore(xnc->sc->mp, dp ? dp->i_ino : NULLFSINO,
			ino, delta, __return_address);

	if (!xnc->nlinks)
		return 0;

	error = xchk_nlinks_get_shadow_count(xnc, ino, &nlinks);
	if (error)
		return error;

	nlinks += delta;

	error = xfbma_set(xnc->nlinks, ino, &nlinks);
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

/*
 * Apply a link count change from the regular filesystem into our shadow link
 * count structure.
 */
static int
xchk_nlinks_mod_inode(
	struct notifier_block		*nb,
	unsigned long			arg,
	void				*data)
{
	struct xfs_nlink_mod_params	*p = data;
	struct xchk_nlinks		*xnc;
	int				error;

	xnc = container_of(nb, struct xchk_nlinks, mod_hook);

	xchk_iscan_lock(&xnc->iscan);
	if (!xchk_iscan_marked(&xnc->iscan, p->dp) || xnc->hook_dead)
		goto out_unlock;

	error = xchk_nlinks_update_incore(xnc, p->dp, p->ino, p->delta);
	if (error)
		xnc->hook_dead = true;

out_unlock:
	xchk_iscan_unlock(&xnc->iscan);
	return NOTIFY_DONE;
}

struct xchk_walk_dir {
	struct dir_context	dir_iter;
	struct xchk_nlinks	*xnc;
	struct xfs_inode	*dp;
};

/* Bump the shadow link count for every inode referenced by this dir. */
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
	struct xchk_nlinks	*xnc;
	int			error = -ECANCELED;

	xwd = container_of(dir_iter, struct xchk_walk_dir, dir_iter);
	xnc = xwd->xnc;

	/* Update the shadow link counts if we haven't already failed. */
	xchk_iscan_lock(&xnc->iscan);
	if (xnc->hook_dead) {
		xchk_set_incomplete(xnc->sc);
		goto out_unlock;
	}

	error = xchk_nlinks_update_incore(xnc, xwd->dp, ino, 1);
	if (error) {
		xchk_set_incomplete(xnc->sc);
		xnc->hook_dead = true;
	}

out_unlock:
	xchk_iscan_unlock(&xnc->iscan);
	return error;
}

/* Bump the link counts of every entry in this directory. */
STATIC int
xchk_nlinks_dir(
	struct xchk_nlinks	*xnc,
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

out:
	xfs_iunlock(dp, XFS_IOLOCK_SHARED);
	return error;
}

/* If this looks like a valid pointer, count it. */
static inline int
xchk_nlinks_metafile(
	struct xchk_nlinks	*xnc,
	xfs_ino_t		ino)
{
	if (!xfs_verify_ino(xnc->sc->mp, ino))
		return 0;

	return xchk_nlinks_update_incore(xnc, NULL, ino, 1);
}

/* Bump the link counts of metadata files rooted in the superblock. */
STATIC int
xchk_nlinks_metafiles(
	struct xchk_nlinks	*xnc)
{
	struct xfs_mount	*mp = xnc->sc->mp;
	int			error = -ECANCELED;

	xchk_iscan_lock(&xnc->iscan);
	if (xnc->hook_dead) {
		xchk_set_incomplete(xnc->sc);
		goto out_unlock;
	}

	error = xchk_nlinks_metafile(xnc, mp->m_sb.sb_rbmino);
	if (error)
		goto out_error;

	error = xchk_nlinks_metafile(xnc, mp->m_sb.sb_rsumino);
	if (error)
		goto out_error;

	error = xchk_nlinks_metafile(xnc, mp->m_sb.sb_uquotino);
	if (error)
		goto out_error;

	error = xchk_nlinks_metafile(xnc, mp->m_sb.sb_gquotino);
	if (error)
		goto out_error;

	error = xchk_nlinks_metafile(xnc, mp->m_sb.sb_pquotino);

out_error:
	if (error) {
		xchk_set_incomplete(xnc->sc);
		xnc->hook_dead = true;
	}
out_unlock:
	xchk_iscan_unlock(&xnc->iscan);
	return error;
}

/* Walk all directories and count inode links. */
STATIC int
xchk_nlinks_collect(
	struct xchk_nlinks	*xnc)
{
	struct xfs_scrub	*sc = xnc->sc;
	struct xchk_iscan	*iscan = &xnc->iscan;
	struct xfs_inode	*ip;
	int			flags = XFS_IGET_UNTRUSTED;
	unsigned int		retries = 20;
	int			error;

	/* Count the rt and quota files if they're rooted in the superblock. */
	if (!xfs_has_metadir(sc->mp)) {
		error = xchk_nlinks_metafiles(xnc);
		if (error)
			return error;
	}

	while (!(error = xchk_iscan_advance(iscan, sc->tp))) {
		if (iscan->cursor_ino == NULLFSINO ||
		    xchk_should_terminate(sc, &error))
			break;

		error = xfs_iget(sc->mp, sc->tp, iscan->cursor_ino, flags, 0,
				&ip);
		switch (error) {
		case 0:
			if (S_ISDIR(VFS_I(ip)->i_mode))
				error = xchk_nlinks_dir(xnc, ip);
			xchk_irele(sc, ip);
			if (error)
				return error;
			retries = 20;
			break;
		case -ENOENT:
			/*¬
			 * It's possible that this inode has lost all of its
			 * links but hasn't yet been inactivated.  Try to push
			 * it towards inactivation.
			 */
			xfs_inodegc_flush(xnc->sc->mp);
			fallthrough;
		case -EINVAL:
			/*
			 * We thought the inode was allocated, but iget failed
			 * to find it.  This could be because the inobt lookup
			 * failed, or because there's an incore inode that
			 * thinks it's marked free.  Either way, we back up
			 * one inode and try to advance the cursor.
			 */
			xchk_iscan_retry(iscan);
			if (--retries == 0) {
				xchk_set_incomplete(sc);
				return -ECANCELED;
			}
			delay(HZ / 10);
			break;
		default:
			return error;
		}
	}

	return error;
}

/* Check the link count against an inode. */
STATIC int
xchk_nlinks_compare_inode(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	xfs_ino_t		ino,
	void			*data)
{
	struct xchk_nlinks	*xnc = data;
	struct xfs_inode	*ip = NULL;
	xfs_nlink_t		live_nlink;
	xfs_nlink_t		ino_nlink = 0;
	int			error;

	error = xfs_iget(mp, tp, ino, XFS_IGET_UNTRUSTED, XFS_ILOCK_SHARED,
			&ip);
	if (error == -ENOENT || error == -EINVAL) {
		/* Inode wasn't found, so we'll compare against zero nlink. */
		error = 0;
	}
	if (error)
		return error;

	xchk_iscan_lock(&xnc->iscan);
	if (xnc->hook_dead) {
		xchk_set_incomplete(xnc->sc);
		error = -ECANCELED;
		goto out_unlock;
	}
	error = xchk_nlinks_get_shadow_count(xnc, ino, &live_nlink);
	if (error) {
		xchk_set_incomplete(xnc->sc);
		xnc->hook_dead = true;
		goto out_unlock;
	}

	if (ip)
		ino_nlink = VFS_I(ip)->i_nlink;

	if (live_nlink != ino_nlink) {
		trace_xchk_nlinks_compare_inode(mp, ino, ino_nlink, live_nlink);
		xchk_ino_set_corrupt(xnc->sc, ino);
		error = -ECANCELED;
	}

out_unlock:
	xchk_iscan_unlock(&xnc->iscan);
	if (ip) {
		xfs_iunlock(ip, XFS_ILOCK_SHARED);
		xchk_irele(xnc->sc, ip);
	}
	return error;
}

/*
 * Walk all the observed link counts, and make sure there's a matching incore
 * inode and that its counts match ours.
 */
STATIC int
xchk_nlinks_walk_observations(
	struct xchk_nlinks	*xnc)
{
	struct xfs_scrub	*sc = xnc->sc;
	uint64_t		nr = 0;
	xfs_nlink_t		nlink;
	int			error;

	if (!xnc->nlinks)
		return 0;

	xchk_iscan_lock(&xnc->iscan);
	while (!(error = xfbma_iter_get(xnc->nlinks, &nr, &nlink))) {
		xchk_iscan_unlock(&xnc->iscan);

		if (xchk_should_terminate(xnc->sc, &error))
			return error;

		error = xchk_nlinks_compare_inode(sc->mp, sc->tp, nr - 1, xnc);
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

/* Compare the link counts we observed against the live information. */
STATIC int
xchk_nlinks_compare_counts(
	struct xchk_nlinks	*xnc)
{
	struct xfs_scrub	*sc = xnc->sc;
	int			error;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return 0;

	error = xfs_iwalk(sc->mp, sc->tp, 0, XFS_IWALK_METADIR,
			xchk_nlinks_compare_inode, 0, xnc);
	if (error)
		return error;

	/* Walk all the observed link counts and compare to the incore ones. */
	return xchk_nlinks_walk_observations(xnc);
}

/* Tear down everything associated with a nlinks check. */
static void
xchk_nlinks_teardown_scan(
	struct xchk_nlinks	*xnc)
{
	/* Discourage any hook functions that might be running. */
	xchk_iscan_lock(&xnc->iscan);
	xnc->hook_dead = true;
	xchk_iscan_unlock(&xnc->iscan);

	/*
	 * As noted above, the apply hook is responsible for cleaning up the
	 * shadow dquot accounting data when a transaction completes.  The mod
	 * hook must be removed before the apply hook so that we don't
	 * mistakenly leave an active shadow account for the mod hook to get
	 * its hands on.  No hooks should be running by the time this function
	 * completes.
	 */
	xfs_hook_del(&xnc->sc->mp->m_nlink_mod_hooks, &xnc->mod_hook);

	xfbma_destroy(xnc->nlinks);
	xnc->nlinks = NULL;

	xchk_iscan_finish(&xnc->iscan);
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
	struct xchk_nlinks	*xnc)
{
	int			error;

	ASSERT(xnc->sc == NULL);
	xnc->sc = sc;

	xnc->hook_dead = false;
	xchk_iscan_start(&xnc->iscan);

	error = -ENOMEM;
	xnc->nlinks = xfbma_init("link counts", sizeof(xfs_nlink_t));
	if (!xnc->nlinks)
		goto out_teardown;

	/*
	 * Hook into the bumplink/droplink code.  The hook only triggers for
	 * inodes that were already scanned, and the scanner thread takes each
	 * inode's ILOCK, which means that any in-progress inode updates will
	 * finish before we can scan the inode.
	 */
	error = xfs_hook_add(&sc->mp->m_nlink_mod_hooks, &xnc->mod_hook,
			xchk_nlinks_mod_inode);
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
	struct xchk_nlinks	*xnc = sc->buf;
	int			error = 0;

	/* Check link counts on the live filesystem. */
	error = xchk_nlinks_setup_scan(sc, xnc);
	if (error)
		return error;

	/* Walk all inodes, picking up link count information. */
	error = xchk_nlinks_collect(xnc);
	if (!xchk_xref_process_error(sc, 0, 0, &error))
		return error;

	/* Compare link counts. */
	error = xchk_nlinks_compare_counts(xnc);
	if (!xchk_xref_process_error(sc, 0, 0, &error))
		return error;

	return 0;
}
