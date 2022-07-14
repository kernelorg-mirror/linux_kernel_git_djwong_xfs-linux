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
#include "scrub/readdir.h"
#include "scrub/parent.h"

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
	struct xfs_scrub	*sc;
	xfs_ino_t		ino;
	xfs_nlink_t		nlink;
};

/* Look for a single entry in a directory pointing to an inode. */
STATIC int
xchk_parent_actor(
	struct xfs_scrub	*sc,
	struct xfs_inode	*dp,
	xfs_dir2_dataptr_t	dapos,
	const struct xfs_name	*name,
	xfs_ino_t		ino,
	void			*priv)
{
	struct xchk_parent_ctx	*spc = priv;
	int			error = 0;

	/* Does this name make sense? */
	if (!xfs_dir2_namecheck(name->name, name->len))
		error = -EFSCORRUPTED;
	if (!xchk_fblock_xref_process_error(sc, XFS_DATA_FORK, 0, &error))
		return error;

	if (spc->ino == ino)
		spc->nlink++;

	if (xchk_should_terminate(spc->sc, &error))
		return error;

	return 0;
}

/*
 * Try to iolock the parent dir @dp in shared mode and the child dir @sc->ip
 * exclusively.
 */
int
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
	struct xchk_parent_ctx	spc = {
		.sc		= sc,
		.ino		= sc->ip->i_ino,
		.nlink		= 0,
	};
	struct xfs_inode	*dp = NULL;
	xfs_nlink_t		expected_nlink;
	unsigned int		lock_mode;
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
	if (dp == sc->ip || dp == sc->tempip || !S_ISDIR(VFS_I(dp)->i_mode)) {
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
	lock_mode = xfs_ilock_data_map_shared(dp);
	error = xchk_dir_walk(sc, dp, xchk_parent_actor, &spc);
	xfs_iunlock(dp, lock_mode);
	if (!xchk_fblock_xref_process_error(sc, XFS_DATA_FORK, 0, &error))
		goto out_unlock;

	/*
	 * Ensure that the parent has as many links to the child as the child
	 * thinks it has to the parent.
	 */
	if (spc.nlink != expected_nlink)
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
