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
#include "xfs_health.h"
#include "xfs_swapext.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/iscan.h"
#include "scrub/parent.h"
#include "scrub/readdir.h"
#include "scrub/tempfile.h"

struct xrep_findparent_info {
	/* The directory currently being scanned. */
	struct xfs_inode	*dp;

	/*
	 * Scrub context.  We're looking for a @dp containing a directory
	 * entry pointing to sc->ip->i_ino.
	 */
	struct xfs_scrub	*sc;

	/*
	 * Parent that we've found for sc->ip.  If we're scanning the entire
	 * directory tree, we need this to ensure that we only find /one/
	 * parent directory.
	 */
	xfs_ino_t		found_parent;

	/*
	 * This is set to true if @found_parent was not observed directly from
	 * the directory scan but by noticing a change in dotdot entries after
	 * cycling the sc->ip IOLOCK.
	 */
	bool			parent_tentative;
};

/*
 * If this directory entry points to the scrub target inode, then the directory
 * we're scanning is the parent of the scrub target inode.
 */
STATIC int
xrep_findparent_dirent(
	struct xfs_scrub		*sc,
	struct xfs_inode		*dp,
	xfs_dir2_dataptr_t		dapos,
	const struct xfs_name		*name,
	xfs_ino_t			ino,
	void				*priv)
{
	struct xrep_findparent_info	*fpi = priv;
	int				error = 0;

	if (xchk_should_terminate(fpi->sc, &error))
		return error;

	if (ino != fpi->sc->ip->i_ino)
		return 0;

	/* Ignore garbage directory entry names. */
	if (name->len == 0 || !xfs_dir2_namecheck(name->name, name->len))
		return -EFSCORRUPTED;

	/*
	 * Ignore dotdot and dot entries -- we're looking for parent -> child
	 * links only.
	 */
	if (name->name[0] == '.' && (name->len == 1 ||
				     (name->len == 2 && name->name[1] == '.')))
		return 0;

	/* Uhoh, more than one parent for a dir? */
	if (fpi->found_parent != NULLFSINO &&
	    !(fpi->parent_tentative && fpi->found_parent == fpi->dp->i_ino)) {
		trace_xrep_findparent_dirent(fpi->sc->ip, 0);
		return -EFSCORRUPTED;
	}

	/* We found a potential parent; remember this. */
	trace_xrep_findparent_dirent(fpi->sc->ip, fpi->dp->i_ino);
	fpi->found_parent = fpi->dp->i_ino;
	fpi->parent_tentative = false;
	return 0;
}

/*
 * If this is a directory, walk the dirents looking for any that point to the
 * scrub target inode.
 */
STATIC int
xrep_findparent_walk_directory(
	struct xrep_findparent_info	*fpi)
{
	struct xfs_scrub		*sc = fpi->sc;
	struct xfs_inode		*dp = fpi->dp;
	unsigned int			lock_mode;
	int				error = 0;

	/*
	 * The inode being scanned cannot be its own parent, nor can any
	 * temporary directory we created to stage this repair.
	 */
	if (dp == sc->ip || dp == sc->tempip)
		return 0;

	/*
	 * Similarly, temporary files created to stage a repair cannot be the
	 * parent of this inode.
	 */
	if (xrep_is_tempfile(dp))
		return 0;

	/* Try to lock dp; if we can, we're ready to scan! */
	if (!xfs_ilock_nowait(dp, XFS_IOLOCK_SHARED)) {
		xfs_ino_t	orig_parent, new_parent;

		/*
		 * We may have to drop the lock on sc->ip to try to lock dp.
		 * Therefore, look up the old dotdot entry for sc->ip so that
		 * we can compare it after we re-lock sc->ip.
		 */
		orig_parent = xrep_dotdot_lookup(sc);

		error = xchk_parent_lock_two_dirs(sc, dp);
		if (error)
			return error;

		/*
		 * It is possible that sc->ip got moved elsewhere in the
		 * directory tree if we dropped sc->ip to grab dp.  Note that
		 * rename operations replace the dotdot entry without checking
		 * the old value.
		 *
		 * If the dotdot entry was wrong but there really was only one
		 * parent of sc->ip, then the dotdot entry could now be
		 * correct.  Record this new parent as a tentative parent and
		 * keep scanning.  If there are more parents of this directory,
		 * we must not touch anything.
		 */
		new_parent = xrep_dotdot_lookup(sc);

		if (orig_parent != new_parent || VFS_I(sc->ip)->i_nlink == 0) {
			fpi->found_parent = new_parent;
			fpi->parent_tentative = true;
		}
	}

	/*
	 * If this directory is known to be sick, we cannot scan it reliably
	 * and must abort.
	 */
	if (xfs_inode_has_sickness(dp, XFS_SICK_INO_CORE |
				       XFS_SICK_INO_BMBTD |
				       XFS_SICK_INO_DIR)) {
		error = -EFSCORRUPTED;
		goto out_unlock;
	}

	/*
	 * Scan the directory to see if there it contains an entry pointing to
	 * the directory that we are repairing.
	 */
	lock_mode = xfs_ilock_data_map_shared(dp);

	/*
	 * We cannot complete our parent pointer scan if a directory looks as
	 * though it has been zapped by the inode record repair code.
	 */
	if (xchk_dir_looks_zapped(dp))
		error = -EFSCORRUPTED;
	if (!error)
		error = xchk_dir_walk(sc, dp, xrep_findparent_dirent, fpi);
	xfs_iunlock(dp, lock_mode);
	if (error)
		goto out_unlock;

out_unlock:
	xfs_iunlock(dp, XFS_IOLOCK_SHARED);
	return error;
}

/*
 * Confirm that the directory @parent_ino actually contains a directory entry
 * pointing to the child @sc->ip->ino.  This function returns one of several
 * ways:
 *
 * Returns 0 with @parent_ino unchanged if the parent was confirmed.
 * Returns 0 with a different @parent_ino if we had to cycle inode locks to
 * walk the alleged parent and the child's '..' entry was changed in the mean
 * time.
 * Returns 0 with @parent_ino set to NULLFSINO if the parent was not valid.
 * Returns the usual negative errno if something else happened.
 */
int
xrep_parent_confirm(
	struct xfs_scrub	*sc,
	xfs_ino_t		*parent_ino)
{
	struct xrep_findparent_info fpi = {
		.sc		= sc,
		.found_parent	= NULLFSINO,
	};
	int			error;

	/*
	 * The root directory always points to itself.  Unlinked dirs can point
	 * anywhere, so we point them at the root dir too.
	 */
	if (sc->ip == sc->mp->m_rootip || VFS_I(sc->ip)->i_nlink == 0) {
		*parent_ino = sc->mp->m_sb.sb_rootino;
		return 0;
	}

	/* Reject garbage parent inode numbers and self-referential parents. */
	if (*parent_ino == NULLFSINO)
	       return 0;
	if (!xfs_verify_dir_ino(sc->mp, *parent_ino) ||
	    *parent_ino == sc->ip->i_ino) {
		*parent_ino = NULLFSINO;
		return 0;
	}

	error = xchk_iget(sc, *parent_ino, &fpi.dp);
	if (error)
		return error;

	if (!S_ISDIR(VFS_I(fpi.dp)->i_mode)) {
		*parent_ino = NULLFSINO;
		goto out_rele;
	}

	error = xrep_findparent_walk_directory(&fpi);
	if (error)
		goto out_rele;

	*parent_ino = fpi.found_parent;
out_rele:
	xchk_irele(sc, fpi.dp);
	return error;
}

/* Check the dentry cache to see if knows of a parent for the scrub target. */
xfs_ino_t
xrep_parent_from_dcache(
	struct xfs_scrub	*sc)
{
	struct inode		*pip = NULL;
	struct dentry		*dentry, *parent;
	xfs_ino_t		ret = NULLFSINO;

	dentry = d_find_alias(VFS_I(sc->ip));
	if (!dentry)
		goto out;

	parent = dget_parent(dentry);
	if (!parent)
		goto out_dput;

	if (parent->d_sb != sc->ip->i_mount->m_super) {
		dput(parent);
		goto out_dput;
	}

	pip = igrab(d_inode(parent));
	dput(parent);

	if (S_ISDIR(pip->i_mode)) {
		trace_xrep_findparent_from_dcache(sc->ip, XFS_I(pip)->i_ino);
		ret = XFS_I(pip)->i_ino;
	}

	xchk_irele(sc, XFS_I(pip));

out_dput:
	dput(dentry);
out:
	return ret;
}

/*
 * Scan the entire filesystem looking for a parent inode for the inode being
 * scrubbed.  @sc->ip must not be the root of a directory tree.
 *
 * Returns 0 with @parent_ino set to the parent that we found, or the current
 * value of the child's '..' entry, if it changed when we had to drop the
 * child's IOLOCK.
 * Returns 0 with @parent_ino set to NULLFSINO if we didn't find anything.
 * Returns the usual negative errno if something else happened.
 */
int
xrep_parent_scan(
	struct xfs_scrub		*sc,
	xfs_ino_t			*parent_ino)
{
	struct xrep_findparent_info	fpi = {
		.sc			= sc,
		.found_parent		= NULLFSINO,
	};
	struct xchk_iscan		iscan = { };
	int				ret;

	/*
	 * The caller holds a non-empty transaction and a directory ILOCK.
	 * Hence we cannot block the system indefinitely in iget, so we will
	 * retry rapidly for up to five seconds before aborting the operation.
	 */
	iscan.iget_nowait = true;
	xchk_iscan_start(&iscan, 5000, 1);

	while ((ret = xchk_iscan_iter(sc, &iscan, &fpi.dp)) == 1) {
		if (S_ISDIR(VFS_I(fpi.dp)->i_mode))
			ret = xrep_findparent_walk_directory(&fpi);
		else
			ret = 0;
		xchk_iscan_mark_visited(&iscan, fpi.dp);
		xchk_irele(sc, fpi.dp);
		if (ret)
			break;

		if (xchk_should_terminate(sc, &ret))
			break;
	}
	xchk_iscan_finish(&iscan);
	if (ret)
		return ret;

	*parent_ino = fpi.found_parent;
	return 0;
}

/*
 * If we're the root of a directory tree, we are our own parent.  If we're an
 * unlinked directory, the parent /won't/ have a link to us.  Set the parent
 * directory to the root for both cases.  Returns NULLFSINO if we don't know
 * what to do.
 */
xfs_ino_t
xrep_parent_self_reference(
	struct xfs_scrub	*sc)
{
	if (sc->ip->i_ino == sc->mp->m_sb.sb_rootino)
		return sc->mp->m_sb.sb_rootino;

	if (VFS_I(sc->ip)->i_nlink == 0)
		return sc->mp->m_sb.sb_rootino;

	return NULLFSINO;
}

/*
 * Repairing The Directory Parent Pointer
 * ======================================
 *
 * Currently, only directories support parent pointers (in the form of '..'
 * entries), so we simply scan the filesystem and update the '..' entry.
 *
 * Note that because the only parent pointer is the dotdot entry, we won't
 * touch an unhealthy directory, since the directory repair code is perfectly
 * capable of rebuilding a directory with the proper parent inode.
 */

/* Replace a directory's parent '..' pointer. */
STATIC int
xrep_parent_reset_dir(
	struct xfs_scrub	*sc,
	xfs_ino_t		parent_ino)
{
	unsigned int		spaceres;
	int			error;

	trace_xrep_parent_reset_dir(sc->ip, parent_ino);

	/*
	 * Reserve more space just in case we have to expand the dir.  We're
	 * allowed to exceed quota to repair inconsistent metadata.
	 */
	spaceres = XFS_RENAME_SPACE_RES(sc->mp, 2);
	error = xfs_trans_reserve_more_inode(sc->tp, sc->ip, spaceres, 0,
			true);
	if (error)
		return error;

	/* Replace the dotdot entry. */
	return xfs_dir_replace(sc->tp, sc->ip, &xfs_name_dotdot, parent_ino,
			spaceres);
}

int
xrep_parent(
	struct xfs_scrub	*sc)
{
	xfs_ino_t		parent_ino, curr_parent;
	unsigned int		sick, checked;
	int			error;

	/*
	 * Avoid sick directories.  The parent pointer scrubber dropped the
	 * ILOCK and MMAPLOCK, but we still hold IOLOCK_EXCL on the directory.
	 * There shouldn't be anyone else clearing the directory's sick status.
	 */
	xfs_inode_measure_sickness(sc->ip, &sick, &checked);
	if (sick & XFS_SICK_INO_DIR)
		return -EFSCORRUPTED;

	parent_ino = xrep_parent_self_reference(sc);
	if (parent_ino != NULLFSINO)
		goto reset_parent;

	/* Does the VFS dcache have an answer for us? */
	parent_ino = xrep_parent_from_dcache(sc);
	error = xrep_parent_confirm(sc, &parent_ino);
	if (!error && parent_ino != NULLFSINO)
		goto reset_parent;

	/* Scan the entire filesystem for a parent. */
	error = xrep_parent_scan(sc, &parent_ino);
	if (error)
		return error;
	if (parent_ino == NULLFSINO)
		return -EFSCORRUPTED;

reset_parent:
	/* If the '..' entry is already set to the parent inode, we're done. */
	curr_parent = xrep_dotdot_lookup(sc);
	if (curr_parent != NULLFSINO && curr_parent == parent_ino)
		return 0;

	/* Last chance to abort before we start committing fixes. */
	if (xchk_should_terminate(sc, &error))
		return error;

	/* Re-take the ILOCK, we're going to need it to modify the dir. */
	xchk_ilock(sc, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(sc->tp, sc->ip, 0);

	return xrep_parent_reset_dir(sc, parent_ino);
}
