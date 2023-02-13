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
#include "scrub/xfile.h"
#include "scrub/xfarray.h"
#include "scrub/xfblob.h"
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

/* Deferred parent pointer entry that we saved for later. */
struct xchk_pptr {
	/* Cookie for retrieval of the pptr name. */
	xfblob_cookie			name_cookie;

	/* Parent pointer attr key. */
	xfs_ino_t			p_ino;
	uint32_t			p_gen;
	xfs_dir2_dataptr_t		p_diroffset;

	/* Length of the pptr name. */
	uint8_t				namelen;
};

struct xchk_pptrs {
	struct xfs_scrub	*sc;

	/* Scratch buffer for scanning pptr xattrs */
	struct xfs_parent_name_irec pptr;

	/* Fixed-size array of xchk_pptr structures. */
	struct xfarray		*pptr_entries;

	/* Blobs containing parent pointer names. */
	struct xfblob		*pptr_names;

	/* Parent of this directory. */
	xfs_ino_t		parent_ino;

	/* If we've cycled the ILOCK, we must revalidate all deferred pptrs. */
	bool			need_revalidate;

	/* xattr key and da args for parent pointer revalidation. */
	struct xfs_parent_scratch pptr_scratch;

	/* Name buffer for revalidation. */
	uint8_t			namebuf[MAXNAMELEN];
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
		struct xchk_pptr	save_pp = {
			.p_ino		= pp->pptr.p_ino,
			.p_gen		= pp->pptr.p_gen,
			.p_diroffset	= pp->pptr.p_diroffset,
			.namelen	= pp->pptr.p_namelen,
		};

		/* Couldn't lock the inode, so save the pptr for later. */
		trace_xchk_parent_defer(sc->ip, pp->pptr.p_name,
				pp->pptr.p_namelen, dp->i_ino);

		error = xfblob_store(pp->pptr_names, &save_pp.name_cookie,
				pp->pptr.p_name, pp->pptr.p_namelen);
		if (xchk_fblock_process_error(sc, XFS_ATTR_FORK, 0, &error))
			goto out_rele;

		error = xfarray_append(pp->pptr_entries, &save_pp);
		if (xchk_fblock_process_error(sc, XFS_ATTR_FORK, 0, &error))
			goto out_rele;

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

/*
 * Revalidate a parent pointer that we collected in the past but couldn't check
 * because of lock contention.  Returns 0 if the parent pointer is still valid,
 * -ENOENT if it has gone away on us, or a negative errno.
 */
STATIC int
xchk_parent_revalidate_pptr(
	struct xchk_pptrs	*pp)
{
	struct xfs_scrub	*sc = pp->sc;
	int			namelen;

	namelen = xfs_parent_lookup(sc->tp, sc->ip, &pp->pptr, pp->namebuf,
			MAXNAMELEN, &pp->pptr_scratch);
	if (namelen == -ENOATTR) {
		/*  Parent pointer went away, nothing to revalidate. */
		return -ENOENT;
	}
	if (namelen < 0 && namelen != -EEXIST)
		return namelen;

	/*
	 * The dirent name changed length while we were unlocked.  No need
	 * to revalidate this.
	 */
	if (namelen != pp->pptr.p_namelen)
		return -ENOENT;

	/* The dirent name itself changed; there's nothing to revalidate. */
	if (memcmp(pp->namebuf, pp->pptr.p_name, pp->pptr.p_namelen))
		return -ENOENT;
	return 0;
}

/*
 * Check a parent pointer the slow way, which means we cycle locks a bunch
 * and put up with revalidation until we get it done.
 */
STATIC int
xchk_parent_slow_pptr(
	struct xchk_pptrs	*pp,
	struct xchk_pptr	*pptr)
{
	struct xfs_scrub	*sc = pp->sc;
	struct xfs_inode	*dp = NULL;
	unsigned int		lockmode;
	int			error;

	/* Restore the saved parent pointer into the irec. */
	pp->pptr.p_ino = pptr->p_ino;
	pp->pptr.p_gen = pptr->p_gen;
	pp->pptr.p_diroffset = pptr->p_diroffset;

	error = xfblob_load(pp->pptr_names, pptr->name_cookie, pp->pptr.p_name,
			pptr->namelen);
	if (error)
		return error;
	pp->pptr.p_name[MAXNAMELEN - 1] = 0;
	pp->pptr.p_namelen = pptr->namelen;

	/* Check that the deferred parent pointer still exists. */
	if (pp->need_revalidate) {
		error = xchk_parent_revalidate_pptr(pp);
		if (error == -ENOENT)
			return 0;
		if (!xchk_fblock_xref_process_error(sc, XFS_ATTR_FORK, 0,
					&error))
			return error;
	}

	error = xchk_parent_iget(pp, &dp);
	if (error)
		return error;
	if (!dp)
		return 0;

	/*
	 * If we can grab both IOLOCK and ILOCK of the alleged parent, we
	 * can proceed with the validation.
	 */
	lockmode = xchk_parent_lock_dir(sc, dp);
	if (lockmode)
		goto check_dirent;

	/*
	 * We couldn't lock the parent dir.  Drop all the locks and try to
	 * get them again, one at a time.
	 */
	xchk_iunlock(sc, sc->ilock_flags);
	pp->need_revalidate = true;

	trace_xchk_parent_slowpath(sc->ip, pp->namebuf, pptr->namelen,
			dp->i_ino);

	while (true) {
		xchk_ilock(sc, XFS_IOLOCK_EXCL);
		if (xfs_ilock_nowait(dp, XFS_IOLOCK_SHARED)) {
			xchk_ilock(sc, XFS_ILOCK_EXCL);
			if (xfs_ilock_nowait(dp, XFS_ILOCK_EXCL)) {
				break;
			}
			xchk_iunlock(sc, XFS_ILOCK_EXCL);
		}
		xchk_iunlock(sc, XFS_IOLOCK_EXCL);

		if (xchk_should_terminate(sc, &error))
			goto out_rele;

		delay(1);
	}
	lockmode = XFS_IOLOCK_SHARED | XFS_ILOCK_EXCL;

	/*
	 * If we didn't already find a parent pointer matching the dotdot
	 * entry, re-query the dotdot entry so that we can validate it.
	 */
	if (pp->parent_ino != NULLFSINO) {
		error = xchk_parent_dotdot(pp);
		if (error)
			goto out_unlock;
	}

	/* Revalidate the parent pointer now that we cycled locks. */
	error = xchk_parent_revalidate_pptr(pp);
	if (error == -ENOENT)
		goto out_unlock;
	if (!xchk_fblock_xref_process_error(sc, XFS_ATTR_FORK, 0, &error))
		goto out_unlock;

check_dirent:
	error = xchk_parent_dirent(pp, dp);
out_unlock:
	xfs_iunlock(dp, lockmode);
out_rele:
	xchk_irele(sc, dp);
	return error;
}

/* Check all the parent pointers that we deferred the first time around. */
STATIC int
xchk_parent_finish_slow_pptrs(
	struct xchk_pptrs	*pp)
{
	xfarray_idx_t		array_cur;
	int			error;

	foreach_xfarray_idx(pp->pptr_entries, array_cur) {
		struct xchk_pptr	pptr;

		if (pp->sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
			return 0;

		error = xfarray_load(pp->pptr_entries, array_cur, &pptr);
		if (error)
			return error;

		error = xchk_parent_slow_pptr(pp, &pptr);
		if (error)
			return error;
	}

	/* Empty out both xfiles now that we've checked everything. */
	xfarray_truncate(pp->pptr_entries);
	xfblob_truncate(pp->pptr_names);
	return 0;
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

	/*
	 * Set up some staging memory for parent pointers that we can't check
	 * due to locking contention.
	 */
	error = xfarray_create(sc->mp, "pptr entries", 0,
			sizeof(struct xchk_pptr), &pp->pptr_entries);
	if (error)
		goto out_pp;

	error = xfblob_create(sc->mp, "pptr names", &pp->pptr_names);
	if (error)
		goto out_entries;

	error = xchk_xattr_walk(sc, sc->ip, xchk_parent_scan_attr, pp);
	if (error == -ECANCELED) {
		error = 0;
		goto out_names;
	}
	if (error)
		goto out_names;

	error = xchk_parent_finish_slow_pptrs(pp);
	if (error)
		goto out_names;

out_names:
	xfblob_destroy(pp->pptr_names);
out_entries:
	xfarray_destroy(pp->pptr_entries);
out_pp:
	kvfree(pp);
	return error;
}
