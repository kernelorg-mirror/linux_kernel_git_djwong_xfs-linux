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
#include "xfs_attr.h"
#include "xfs_parent.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/readdir.h"
#include "scrub/listxattr.h"
#include "scrub/trace.h"

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

/* Count the number of dentries in the parent dir that point to this inode. */
STATIC int
xchk_parent_count_parent_dentries(
	struct xfs_scrub	*sc,
	struct xfs_inode	*parent,
	struct xchk_parent_ctx	*spc)
{
	uint			lock_mode;
	int			error;

	lock_mode = xfs_ilock_data_map_shared(parent);
	error = xchk_dir_walk(sc, parent, xchk_parent_actor, spc);
	xfs_iunlock(parent, lock_mode);
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
	struct xchk_parent_ctx	spc = {
		.sc		= sc,
		.ino		= sc->ip->i_ino,
		.nlink		= 0,
	};
	struct xfs_inode	*dp = NULL;
	xfs_nlink_t		expected_nlink;
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
	error = xchk_parent_count_parent_dentries(sc, dp, &spc);
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

STATIC int xchk_parent_pptr(struct xfs_scrub *sc);

/* Scrub a parent pointer. */
int
xchk_parent(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	xfs_ino_t		parent_ino;
	int			error;

	if (xfs_has_parent(mp))
		return xchk_parent_pptr(sc);

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

/*
 * Checking of Parent Pointers
 * ===========================
 *
 * On filesystems with directory parent pointers, we check the referential
 * integrity by visiting each parent pointer of a child file and checking that
 * the directory referenced by the pointer actually has a dirent pointing back
 * to the child file.
 */

struct xchk_pptrs {
	struct xfs_scrub	*sc;

	/* Scratch buffer for scanning pptr xattrs */
	struct xfs_parent_name_irec pptr;

	/* Parent of this directory. */
	xfs_ino_t		parent_ino;
};

/* Look up the dotdot entry so that we can check it as we walk the pptrs. */
STATIC int
xchk_parent_dotdot(
	struct xchk_pptrs	*pp)
{
	struct xfs_scrub	*sc = pp->sc;
	int			error;

	if (!S_ISDIR(VFS_I(sc->ip)->i_mode)) {
		pp->parent_ino = NULLFSINO;
		return 0;
	}

	/* Look up '..' */
	error = xchk_dir_lookup(sc, sc->ip, &xfs_name_dotdot, &pp->parent_ino,
			NULL);
	if (!xchk_fblock_process_error(sc, XFS_DATA_FORK, 0, &error))
		return error;
	if (!xfs_verify_dir_ino(sc->mp, pp->parent_ino)) {
		xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, 0);
		return 0;
	}

	/* Is this the root dir?  Then '..' must point to itself. */
	if (sc->ip == sc->mp->m_rootip && sc->ip->i_ino != pp->parent_ino)
		xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, 0);

	return 0;
}

/*
 * Try to lock a parent directory for checking dirents.  Returns the inode
 * flags for the locks we now hold, or zero if we failed.
 */
STATIC unsigned int
xchk_parent_lock_dir(
	struct xfs_scrub	*sc,
	struct xfs_inode	*dp)
{
	if (!xfs_ilock_nowait(dp, XFS_IOLOCK_SHARED))
		return 0;

	if (!xfs_ilock_nowait(dp, XFS_ILOCK_SHARED)) {
		xfs_iunlock(dp, XFS_IOLOCK_SHARED);
		return 0;
	}

	if (!xfs_need_iread_extents(&dp->i_df))
		return XFS_IOLOCK_SHARED | XFS_ILOCK_SHARED;

	xfs_iunlock(dp, XFS_ILOCK_SHARED);

	if (!xfs_ilock_nowait(dp, XFS_ILOCK_EXCL)) {
		xfs_iunlock(dp, XFS_IOLOCK_SHARED);
		return 0;
	}

	return XFS_IOLOCK_SHARED | XFS_ILOCK_EXCL;
}

/* Check the forward link (dirent) associated with this parent pointer. */
STATIC int
xchk_parent_dirent(
	struct xchk_pptrs	*pp,
	struct xfs_inode	*dp)
{
	struct xfs_name		xname = {
		.name		= pp->pptr.p_name,
		.len		= pp->pptr.p_namelen,
	};
	struct xfs_scrub	*sc = pp->sc;
	xfs_ino_t		child_ino;
	xfs_dir2_dataptr_t	child_diroffset;
	int			error;

	/*
	 * Use the name attached to this parent pointer to look up the
	 * directory entry in the alleged parent.
	 */
	error = xchk_dir_lookup(sc, dp, &xname, &child_ino, &child_diroffset);
	if (error == -ENOENT) {
		xchk_fblock_xref_set_corrupt(sc, XFS_ATTR_FORK, 0);
		return 0;
	}
	if (!xchk_fblock_xref_process_error(sc, XFS_ATTR_FORK, 0, &error))
		return error;

	/* Does the inode number match? */
	if (child_ino != sc->ip->i_ino) {
		xchk_fblock_xref_set_corrupt(sc, XFS_ATTR_FORK, 0);
		return 0;
	}

	/* Does the directory offset match? */
	if (pp->pptr.p_diroffset != child_diroffset) {
		trace_xchk_parent_bad_dapos(sc->ip, pp->pptr.p_diroffset,
				dp->i_ino, child_diroffset, xname.name,
				xname.len);
		xchk_fblock_xref_set_corrupt(sc, XFS_ATTR_FORK, 0);
		return 0;
	}

	/*
	 * If we're scanning a directory, we should only ever encounter a
	 * single parent pointer, and it should match the dotdot entry.  We set
	 * the parent_ino from the dotdot entry before the scan, so compare it
	 * now.
	 */
	if (!S_ISDIR(VFS_I(sc->ip)->i_mode))
		return 0;

	if (pp->parent_ino != dp->i_ino) {
		xchk_fblock_set_corrupt(sc, XFS_ATTR_FORK, 0);
		return 0;
	}

	pp->parent_ino = NULLFSINO;
	return 0;
}

/* Try to grab a parent directory. */
STATIC int
xchk_parent_iget(
	struct xchk_pptrs		*pp,
	struct xfs_inode		**dpp)
{
	struct xfs_scrub		*sc = pp->sc;
	struct xfs_inode		*ip;
	int				error;

	/* Validate inode number. */
	error = xfs_dir_ino_validate(sc->mp, pp->pptr.p_ino);
	if (error) {
		xchk_fblock_set_corrupt(sc, XFS_ATTR_FORK, 0);
		return -ECANCELED;
	}

	error = xchk_iget(sc, pp->pptr.p_ino, &ip);
	if (error == -EINVAL || error == -ENOENT) {
		xchk_fblock_set_corrupt(sc, XFS_ATTR_FORK, 0);
		return -ECANCELED;
	}
	if (!xchk_fblock_xref_process_error(sc, XFS_ATTR_FORK, 0, &error))
		return error;

	/* The parent must be a directory. */
	if (!S_ISDIR(VFS_I(ip)->i_mode)) {
		xchk_fblock_xref_set_corrupt(sc, XFS_ATTR_FORK, 0);
		goto out_rele;
	}

	/* Validate generation number. */
	if (VFS_I(ip)->i_generation != pp->pptr.p_gen) {
		xchk_fblock_xref_set_corrupt(sc, XFS_ATTR_FORK, 0);
		goto out_rele;
	}

	*dpp = ip;
	return 0;
out_rele:
	xchk_irele(sc, ip);
	return 0;
}

/*
 * Walk an xattr of a file.  If this xattr is a parent pointer, follow it up
 * to a parent directory and check that the parent has a dirent pointing back
 * to us.
 */
STATIC int
xchk_parent_scan_attr(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip,
	unsigned int		attr_flags,
	const unsigned char	*name,
	unsigned int		namelen,
	const void		*value,
	unsigned int		valuelen,
	void			*priv)
{
	struct xchk_pptrs	*pp = priv;
	struct xfs_inode	*dp = NULL;
	const struct xfs_parent_name_rec *rec = (const void *)name;
	unsigned int		lockmode;
	int			error;

	/* Ignore incomplete xattrs */
	if (attr_flags & XFS_ATTR_INCOMPLETE)
		return 0;

	/* Ignore anything that isn't a parent pointer. */
	if (!(attr_flags & XFS_ATTR_PARENT))
		return 0;

	/* Does the ondisk parent pointer structure make sense? */
	if (!xfs_parent_namecheck(sc->mp, rec, namelen, attr_flags)) {
		xchk_fblock_set_corrupt(sc, XFS_ATTR_FORK, 0);
		return -ECANCELED;
	}

	if (!xfs_parent_valuecheck(sc->mp, value, valuelen)) {
		xchk_fblock_set_corrupt(sc, XFS_ATTR_FORK, 0);
		return -ECANCELED;
	}

	xfs_parent_irec_from_disk(&pp->pptr, rec, value, valuelen);

	error = xchk_parent_iget(pp, &dp);
	if (error)
		return error;
	if (!dp)
		return 0;

	/* Try to lock the inode. */
	lockmode = xchk_parent_lock_dir(sc, dp);
	if (!lockmode) {
		xchk_set_incomplete(sc);
		error = -ECANCELED;
		goto out_rele;
	}

	error = xchk_parent_dirent(pp, dp);
	if (error)
		goto out_unlock;

out_unlock:
	xfs_iunlock(dp, lockmode);
out_rele:
	xchk_irele(sc, dp);
	return error;
}

/* Check parent pointers of a file. */
STATIC int
xchk_parent_pptr(
	struct xfs_scrub	*sc)
{
	struct xchk_pptrs	*pp;
	int			error;

	pp = kvzalloc(sizeof(struct xchk_pptrs), XCHK_GFP_FLAGS);
	if (!pp)
		return -ENOMEM;
	pp->sc = sc;

	error = xchk_parent_dotdot(pp);
	if (error)
		goto out_pp;

	error = xchk_xattr_walk(sc, sc->ip, xchk_parent_scan_attr, pp);
	if (error == -ECANCELED) {
		error = 0;
		goto out_pp;
	}
	if (error)
		goto out_pp;

out_pp:
	kvfree(pp);
	return error;
}
