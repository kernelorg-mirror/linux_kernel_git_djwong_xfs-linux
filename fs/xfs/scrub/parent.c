// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_log_format.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "scrub/scrub.h"
#include "scrub/common.h"

/* Set us up to scrub parents. */
int
xchk_setup_parent(
	struct xfs_scrub	*sc)
{
	return xchk_setup_inode_contents(sc, 0);
}

/* Parent pointers */

/* Look for an entry in a parent pointing to this inode. */

struct xchk_parent_ctx {
	struct dir_context	dc;
	struct xfs_scrub	*sc;
	xfs_ino_t		ino;
	xfs_nlink_t		nlink;
	bool			cancelled;
};

/* Look for a single entry in a directory pointing to an inode. */
STATIC int
xchk_parent_actor(
	struct dir_context	*dc,
	const char		*name,
	int			namelen,
	loff_t			pos,
	u64			ino,
	unsigned		type)
{
	struct xchk_parent_ctx	*spc;
	int			error = 0;

	spc = container_of(dc, struct xchk_parent_ctx, dc);
	if (spc->ino == ino)
		spc->nlink++;

	/*
	 * If we're facing a fatal signal, bail out.  Store the cancellation
	 * status separately because the VFS readdir code squashes error codes
	 * into short directory reads.
	 */
	if (xchk_should_terminate(spc->sc, &error))
		spc->cancelled = true;

	return error;
}

/* Count the number of dentries in the parent dir that point to this inode. */
STATIC int
xchk_parent_count_parent_dentries(
	struct xfs_scrub	*sc,
	struct xfs_inode	*parent,
	xfs_nlink_t		*nlink)
{
	struct xchk_parent_ctx	spc = {
		.dc.actor	= xchk_parent_actor,
		.ino		= sc->ip->i_ino,
		.sc		= sc,
	};
	size_t			bufsize;
	loff_t			oldpos;
	uint			lock_mode;
	int			error = 0;

	/*
	 * If there are any blocks, read-ahead block 0 as we're almost
	 * certain to have the next operation be a read there.  This is
	 * how we guarantee that the parent's extent map has been loaded,
	 * if there is one.
	 */
	lock_mode = xfs_ilock_data_map_shared(parent);
	if (parent->i_df.if_nextents > 0)
		error = xfs_dir3_data_readahead(parent, 0, 0);
	xfs_iunlock(parent, lock_mode);
	if (error)
		return error;

	/*
	 * Iterate the parent dir to confirm that there is
	 * exactly one entry pointing back to the inode being
	 * scanned.
	 */
	bufsize = (size_t)min_t(loff_t, XFS_READDIR_BUFSIZE,
			parent->i_disk_size);
	oldpos = 0;
	while (true) {
		error = xfs_readdir(sc->tp, parent, &spc.dc, bufsize);
		if (error)
			goto out;
		if (spc.cancelled) {
			error = -EAGAIN;
			goto out;
		}
		if (oldpos == spc.dc.pos)
			break;
		oldpos = spc.dc.pos;
	}
	*nlink = spc.nlink;
out:
	return error;
}

/*
 * Try to iolock the parent dir @dp in shared mode and the child dir @sc->ip
 * exclusively.
 */
STATIC int
xchk_parent_lock_two_dirs(
	struct xfs_scrub	*sc,
	struct xfs_inode	*dp)
{
	int			error = 0;

	/* Callers shouldn't do this, but protect ourselves anyway. */
	if (dp == sc->ip) {
		ASSERT(dp != sc->ip);
		return -EINVAL;
	}

	xchk_iunlock(sc, sc->ilock_flags);
	while (true) {
		if (xchk_should_terminate(sc, &error))
			return error;

		/*
		 * Normal XFS takes the IOLOCK before grabbing a transaction.
		 * Scrub holds a transaction, which means that we can't block
		 * on either IOLOCK.
		 */
		if (xfs_ilock_nowait(dp, XFS_IOLOCK_SHARED)) {
			if (xchk_ilock_nowait(sc, XFS_IOLOCK_EXCL))
				break;
			xfs_iunlock(dp, XFS_IOLOCK_SHARED);
		}
		delay(1);
	}

	return 0;
}

/*
 * Given the inode number of the alleged parent of the inode being
 * scrubbed, try to validate that the parent has exactly one directory
 * entry pointing back to the inode being scrubbed.
 */
STATIC int
xchk_parent_validate(
	struct xfs_scrub	*sc,
	xfs_ino_t		parent_ino)
{
	struct xfs_inode	*dp = NULL;
	xfs_nlink_t		expected_nlink;
	xfs_nlink_t		nlink;
	int			error = 0;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return 0;

	/* '..' must not point to ourselves. */
	if (sc->ip->i_ino == parent_ino) {
		xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, 0);
		return 0;
	}

	/*
	 * If we're an unlinked directory, the parent /won't/ have a link
	 * to us.  Otherwise, it should have one link.
	 */
	expected_nlink = VFS_I(sc->ip)->i_nlink == 0 ? 0 : 1;

	/*
	 * Grab the parent directory inode.  This must be released before we
	 * cancel the scrub transaction.
	 *
	 * If _iget returns -EINVAL or -ENOENT then the parent inode number is
	 * garbage and the directory is corrupt.  If the _iget returns
	 * -EFSCORRUPTED or -EFSBADCRC then the parent is corrupt which is a
	 *  cross referencing error.  Any other error is an operational error.
	 */
	error = xchk_iget(sc, parent_ino, &dp);
	if (error == -EINVAL || error == -ENOENT) {
		error = -EFSCORRUPTED;
		xchk_fblock_process_error(sc, XFS_DATA_FORK, 0, &error);
		return error;
	}
	if (!xchk_fblock_xref_process_error(sc, XFS_DATA_FORK, 0, &error))
		return error;
	if (dp == sc->ip || !S_ISDIR(VFS_I(dp)->i_mode)) {
		xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, 0);
		goto out_rele;
	}

	/*
	 * We prefer to keep the inode locked while we lock and search its
	 * alleged parent for a forward reference.  If we can grab the iolock
	 * of the alleged parent, then we can move ahead to counting dirents
	 * and checking nlinks.
	 *
	 * However, if we fail to iolock the alleged parent while holding the
	 * child iolock, we have no way to tell if a blocking lock() would
	 * result in an ABBA deadlock.  Release the lock on the child, then
	 * try to lock the alleged parent and trylock the child.
	 */
	if (!xfs_ilock_nowait(dp, XFS_IOLOCK_SHARED)) {
		error = xchk_parent_lock_two_dirs(sc, dp);
		if (error)
			goto out_rele;

		/*
		 * Now that we've locked out updates to the child directory,
		 * re-sample the expected nlink and the '..' dirent.
		 */
		expected_nlink = VFS_I(sc->ip)->i_nlink == 0 ? 0 : 1;

		error = xfs_dir_lookup(sc->tp, sc->ip, &xfs_name_dotdot,
				&parent_ino, NULL);
		if (!xchk_fblock_process_error(sc, XFS_DATA_FORK, 0, &error))
			goto out_unlock;

		/*
		 * After relocking the child directory, the '..' entry points
		 * to a different parent than before.  This means someone moved
		 * the child elsewhere in the directory tree, which means that
		 * the parent link is now correct and we're done.
		 */
		if (parent_ino != dp->i_ino)
			goto out_unlock;
	}

	/* Look for a directory entry in the parent pointing to the child. */
	error = xchk_parent_count_parent_dentries(sc, dp, &nlink);
	if (!xchk_fblock_xref_process_error(sc, XFS_DATA_FORK, 0, &error))
		goto out_unlock;

	/*
	 * Ensure that the parent has as many links to the child as the child
	 * thinks it has to the parent.
	 */
	if (nlink != expected_nlink)
		xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, 0);

out_unlock:
	xfs_iunlock(dp, XFS_IOLOCK_SHARED);
out_rele:
	xchk_irele(sc, dp);
	return error;
}

/* Scrub a parent pointer. */
int
xchk_parent(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	xfs_ino_t		parent_ino;
	int			error;

	/*
	 * If we're a directory, check that the '..' link points up to
	 * a directory that has one entry pointing to us.
	 */
	if (!S_ISDIR(VFS_I(sc->ip)->i_mode))
		return -ENOENT;

	/* We're not a special inode, are we? */
	if (!xfs_verify_dir_ino(mp, sc->ip->i_ino)) {
		xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, 0);
		return 0;
	}

	/*
	 * The VFS grabs a read or write lock via i_rwsem before it reads
	 * or writes to a directory.  If we've gotten this far we've
	 * already obtained IOLOCK_EXCL, which (since 4.10) is the same as
	 * getting a write lock on i_rwsem.  Therefore, it is safe for us
	 * to drop the ILOCK here in order to do directory lookups.
	 */
	xchk_iunlock(sc, XFS_ILOCK_EXCL | XFS_MMAPLOCK_EXCL);

	/* Look up '..' */
	error = xfs_dir_lookup(sc->tp, sc->ip, &xfs_name_dotdot, &parent_ino,
			NULL);
	if (!xchk_fblock_process_error(sc, XFS_DATA_FORK, 0, &error))
		return error;
	if (!xfs_verify_dir_ino(mp, parent_ino)) {
		xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, 0);
		return 0;
	}

	/* Is this the root dir?  Then '..' must point to itself. */
	if (sc->ip == mp->m_rootip) {
		if (sc->ip->i_ino != mp->m_sb.sb_rootino ||
		    sc->ip->i_ino != parent_ino)
			xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, 0);
		return 0;
	}

	return xchk_parent_validate(sc, parent_ino);
}
