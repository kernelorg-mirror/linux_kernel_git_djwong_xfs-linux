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
#include "xfs_dir2_priv.h"
#include "xfs_trans_space.h"
#include "xfs_iwalk.h"
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
xrep_parents_scan_dentry(
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

/* Walk this directory's entries looking for any that point to the target. */
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
	int			locked;
	int			retries = 20;
	int			error;

	if (ino == rps->target_ino)
		return 0;

	/* Grab inode and lock it so we can scan it. */
	error = xfs_iget(mp, tp, ino, XFS_IGET_UNTRUSTED, 0, &dp);
	if (error)
		return error;

	if (!S_ISDIR(VFS_I(dp)->i_mode))
		goto out_rele;

	/*
	 * Try a few times to take the directory IOLOCK.  We have to use
	 * trylock here to avoid an ABBA deadlock with another thread that
	 * might have a parent locked and is asleep trying to lock our target.
	 * The solution for EDEADLOCK is usually to freeze the fs, so try a
	 * few times to get the inode to avoid that heavyweight solution.
	 */
	while (!(locked = xfs_ilock_nowait(dp, XFS_IOLOCK_SHARED)) && --retries)
		delay(HZ / 10);
	if (!locked) {
		error = -EDEADLOCK;
		goto out_rele;
	}

	/*
	 * If there are any blocks, read-ahead block 0 as we're almost certain
	 * to have the next operation be a read there.  This is how we
	 * guarantee that the directory's extent map has been loaded, if there
	 * is one.
	 */
	lock_mode = xfs_ilock_data_map_shared(dp);
	if (dp->i_d.di_nextents > 0)
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
		.dc.actor	= xrep_parents_scan_dentry,
		.data		= data,
		.fn		= fn,
		.target_ino	= target_ino,
	};

	return xfs_iwalk(sc->mp, sc->tp, 0, 0, xrep_parents_scan_inode, 0,
			&rps);
}
