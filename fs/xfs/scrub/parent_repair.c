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
#include "xfs_bit.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_dir2.h"
#include "xfs_bmap_btree.h"
#include "xfs_dir2_priv.h"
#include "xfs_trans_space.h"
#include "xfs_iwalk.h"
#include "xfs_health.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/parent.h"

/*
 * Scanning Directory Trees for Parent Pointers
 * ============================================
 *
 * Walk the inode table looking for directories.  Scan each directory looking
 * for directory entries that point to the target inode.  Call a function on
 * each match.
 */

struct xrep_parents_scan {
	/* Context for scanning all dentries in a directory. */
	struct dir_context	dc;
	void			*data;
	xrep_parents_iter_fn	fn;

	/* Potential parent of the directory we're scanning. */
	xfs_ino_t		*parent_ino;

	/* This is the inode for which we want to find the parent. */
	xfs_ino_t		target_ino;

	/* Directory that we're scanning. */
	struct xfs_inode	*scan_dir;

	/* Errors encountered during scanning. */
	int			scan_error;
};

/*
 * If this directory entry points to the directory we're rebuilding, then the
 * directory we're scanning is the parent.  Call our function.
 *
 * Note that the vfs readdir functions squash the nonzero codes that we return
 * here into a "short" directory read, so the actual error codes are tracked
 * and returned separately.
 */
STATIC int
xrep_parents_scan_dirent(
	struct dir_context	*dc,
	const char		*name,
	int			namelen,
	loff_t			pos,
	u64			ino,
	unsigned		type)
{
	struct xrep_parents_scan *rps;

	rps = container_of(dc, struct xrep_parents_scan, dc);

	if (ino == rps->target_ino) {
		struct xfs_name	xname = { .name = name, .len = namelen };

		rps->scan_error = rps->fn(rps->scan_dir, &xname, type,
					  rps->data);
		if (rps->scan_error)
			return 1;
	}

	return 0;
}

/*
 * If this is a directory, walk the dirents looking for any that point to the
 * target directory.
 */
STATIC int
xrep_parents_scan_inode(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	xfs_ino_t		ino,
	void			*data)
{
	struct xrep_parents_scan *rps = data;
	struct xfs_inode	*dp;
	loff_t			oldpos;
	size_t			bufsize;
	unsigned int		lock_mode;
	int			error;

	if (ino == rps->target_ino)
		return 0;

	/*
	 * Grab inode and lock it so we can scan it.  If the inode is unlinked
	 * or free or corrupt we'll just ignore it, since callers must be able
	 * to handle the case that no parent is ever found.
	 */
	error = xfs_iget(mp, tp, ino, XFS_IGET_UNTRUSTED, 0, &dp);
	if (error)
		return 0;

	if (!S_ISDIR(VFS_I(dp)->i_mode))
		goto out_rele;

	/*
	 * Try to get the parent directory's IOLOCK.  We still hold the child's
	 * IOLOCK in exclusive mode, so we must avoid an ABBA deadlock.
	 */
	error = xchk_ilock_inverted(dp, XFS_IOLOCK_SHARED);
	if (error)
		goto out_rele;

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
		goto out_unlock;

	/*
	 * Scan the directory to see if there it contains an entry pointing to
	 * the directory that we are repairing.
	 */
	rps->scan_dir = dp;
	bufsize = (size_t)min_t(loff_t, XFS_READDIR_BUFSIZE, dp->i_d.di_size);
	oldpos = 0;
	while (true) {
		error = xfs_readdir(tp, dp, &rps->dc, bufsize);
		if (error)
			break;
		if (rps->scan_error) {
			error = rps->scan_error;
			break;
		}
		if (oldpos == rps->dc.pos)
			break;
		oldpos = rps->dc.pos;
	}

out_unlock:
	xfs_iunlock(dp, XFS_IOLOCK_SHARED);
out_rele:
	xfs_irele(dp);
	return error;
}

/* Does the dcache have a parent for this directory? */
xfs_ino_t
xrep_parent_check_dcache(
	struct xfs_inode	*dp)
{
	struct inode		*pip = NULL;
	struct dentry		*dentry, *parent;
	xfs_ino_t		ret = NULLFSINO;

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));

	dentry = d_find_alias(VFS_I(dp));
	if (!dentry)
		goto out;

	parent = dget_parent(dentry);
	if (!parent)
		goto out_dput;

	pip = igrab(d_inode(parent));
	dput(parent);

	ret = pip->i_ino;
	xfs_irele(XFS_I(pip));

out_dput:
	dput(dentry);
out:
	return ret;
}

/* Is this an acceptable parent for the inode we're scrubbing? */
bool
xrep_parent_acceptable(
	struct xfs_scrub	*sc,
	xfs_ino_t		ino)
{
	return ino != NULLFSINO && ino != 0 && ino != sc->ip->i_ino &&
		xfs_verify_dir_ino(sc->mp, ino);
}

/*
 * Scan the directory tree to find the directory entries that point to this
 * inode.
 */
int
xrep_scan_for_parents(
	struct xfs_scrub	*sc,
	xfs_ino_t		target_ino,
	xrep_parents_iter_fn	fn,
	void			*data)
{
	struct xrep_parents_scan rps = {
		.dc.actor	= xrep_parents_scan_dirent,
		.data		= data,
		.fn		= fn,
		.target_ino	= target_ino,
	};

	return xfs_iwalk(sc->mp, sc->tp, 0, 0, xrep_parents_scan_inode, 0,
			&rps);
}

/*
 * Repairing Parent Pointers
 * =========================
 *
 * Currently, only directories support parent pointers (in the form of '..'
 * entries), so we simply scan the filesystem and update the '..' entry.
 *
 * Note that because the only parent pointer is the dotdot entry, we won't
 * touch an unhealthy directory, since the directory repair code is perfectly
 * capable of rebuilding a directory with the proper parent inode.
 */
struct xrep_parent {
	struct xfs_scrub	*sc;

	/* Potential parent pointer. */
	xfs_ino_t		parent_ino;
};

/*
 * If this directory entry points to the directory we're rebuilding, then the
 * directory we're scanning is the parent.  Remember the parent.
 */
STATIC int
xrep_parent_absorb(
	struct xfs_inode	*dp,
	struct xfs_name		*name,
	unsigned int		dtype,
	void			*data)
{
	struct xrep_parent	*rp = data;
	int			error = 0;

	/* Uhoh, more than one parent for a dir? */
	if (rp->parent_ino != NULLFSINO)
		return -EFSCORRUPTED;

	if (xchk_should_terminate(rp->sc, &error))
		return error;

	/* We found a potential parent; remember this. */
	rp->parent_ino = dp->i_ino;
	return 0;
}

/*
 * Move the current file to the orphanage.  The caller must not hold any locks
 * on the orphanage and must hold only the IOLOCK on sc->ip.  The function
 * returns with both inodes joined, ILOCKed, and dirty.
 */
STATIC int
xrep_move_to_orphanage(
	struct xfs_scrub	*sc)
{
	struct xfs_name		xname;
	unsigned char		fname[MAXNAMELEN + 1];
	struct xfs_inode	*dp = sc->tempip;
	struct xfs_mount	*mp = sc->mp;
	xfs_ino_t		ino;
	unsigned int		incr = 0;
	unsigned int		linkres, dotdotres;
	int			error;

	/* No orphanage?  We can't fix this. */
	if (!sc->tempip)
		return -EFSCORRUPTED;

	xname.name = fname;
	xname.len = snprintf(fname, sizeof(fname), "%llu", sc->ip->i_ino);
	xname.type = xfs_mode_to_ftype(VFS_I(sc->ip)->i_mode);

	/* Make sure the filename is unique in the lost+found. */
	error = xfs_dir_lookup(sc->tp, dp, &xname, &ino, NULL);
	while (error == 0 && incr < 10000) {
		xname.len = snprintf(fname, sizeof(fname), "%llu.%d",
				sc->ip->i_ino, ++incr);
		error = xfs_dir_lookup(sc->tp, dp, &xname, &ino, NULL);
	}
	if (error == 0) {
		/* We already have 10,000 entries in the orphanage? */
		return -EFSCORRUPTED;
	}
	if (error != -ENOENT)
		return error;

	trace_xrep_move_orphanage(sc->ip, &xname, dp->i_ino);

	/*
	 * Reserve enough space to add a directory entry to the orphanage and
	 * update the dotdot entry.
	 */
	linkres = XFS_LINK_SPACE_RES(mp, xname.len);
	dotdotres = XFS_RENAME_SPACE_RES(mp, 2);
	error = xfs_trans_reserve_more(sc->tp, linkres + dotdotres, 0);
	if (error)
		return error;

	xfs_lock_two_inodes(dp, XFS_ILOCK_EXCL, sc->ip, XFS_ILOCK_EXCL);
	sc->ilock_flags |= XFS_ILOCK_EXCL;
	sc->temp_ilock_flags |= XFS_ILOCK_EXCL;

	xfs_trans_ijoin(sc->tp, dp, 0);
	xfs_trans_ijoin(sc->tp, sc->ip, 0);

	/*
	 * Create the new name in the orphanage, and bump the link count of
	 * the orphanage if we just added a directory.
	 */
	error = xfs_dir_createname(sc->tp, dp, &xname, sc->ip->i_ino,
			linkres);
	if (error)
		return error;

	xfs_trans_ichgtime(sc->tp, dp, XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
	if (S_ISDIR(VFS_I(sc->ip)->i_mode))
		xfs_bumplink(sc->tp, dp);
	xfs_trans_log_inode(sc->tp, dp, XFS_ILOG_CORE);

	/* Replace the dotdot entry. */
	return xfs_dir_replace(sc->tp, sc->ip, &xfs_name_dotdot, dp->i_ino,
			dotdotres);
}

int
xrep_parent(
	struct xfs_scrub	*sc)
{
	struct xrep_parent	rp = {
		.sc		= sc,
		.parent_ino	= NULLFSINO,
	};
	unsigned int		sick, checked;
	unsigned int		spaceres;
	int			error;

	/*
	 * Avoid sick directories.  The parent pointer scrubber dropped the
	 * ILOCK, but we still hold IOLOCK_EXCL on the directory, so there
	 * shouldn't be anyone else clearing the directory's sick status.
	 */
	xfs_inode_measure_sickness(sc->ip, &sick, &checked);
	if (sick & XFS_SICK_INO_DIR)
		return -EFSCORRUPTED;

	/*
	 * Ask the dcache who it thinks the parent might be.  If that doesn't
	 * pass muster, scan the entire filesystem for the directory's parent.
	 */
	rp.parent_ino = xrep_parent_check_dcache(sc->ip);
	if (!xrep_parent_acceptable(sc, rp.parent_ino)) {
		error = xrep_scan_for_parents(sc, sc->ip->i_ino,
				xrep_parent_absorb, &rp);
		if (error)
			return error;
	}

	/* If we still don't have a parent, move it to lost+found. */
	if (!xrep_parent_acceptable(sc, rp.parent_ino))
		return xrep_move_to_orphanage(sc);

	trace_xrep_parent_dir(sc->ip, rp.parent_ino);

	/* Reserve more space just in case we have to expand the dir. */
	spaceres = XFS_RENAME_SPACE_RES(sc->mp, 2);
	error = xfs_trans_reserve_more(sc->tp, spaceres, 0);
	if (error)
		return error;

	/* Re-take the ILOCK, we're going to need it to modify the dir. */
	sc->ilock_flags |= XFS_ILOCK_EXCL;
	xfs_ilock(sc->ip, XFS_ILOCK_EXCL);

	/* Replace the dotdot entry. */
	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	return xfs_dir_replace(sc->tp, sc->ip, &xfs_name_dotdot, rp.parent_ino,
			spaceres);
}
