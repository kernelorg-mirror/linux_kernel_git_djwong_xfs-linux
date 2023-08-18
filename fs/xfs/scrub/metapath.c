// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
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
#include "xfs_imeta.h"
#include "xfs_imeta_utils.h"
#include "xfs_quota.h"
#include "xfs_qm.h"
#include "xfs_dir2.h"
#include "xfs_parent.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans_space.h"
#include "xfs_attr.h"
#include "xfs_rtgroup.h"
#include "xfs_rtrmap_btree.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/readdir.h"
#include "scrub/repair.h"

/*
 * Metadata Directory Tree Paths
 * =============================
 *
 * A filesystem with metadir enabled expects to find metadata structures
 * attached to files that are accessible by walking a path down the metadata
 * directory tree.  Given the metadir path and the incore inode storing the
 * metadata, this scrubber ensures that the ondisk metadir path points to the
 * ondisk inode represented by the incore inode.
 */

struct xchk_metapath {
	struct xfs_scrub		*sc;

	/* Name for lookup */
	struct xfs_name			xname;

	/* Directory update for repairs */
	struct xfs_dir_update		du;

	/* Path for this metadata file */
	const struct xfs_imeta_path	*path;

	/* Directory parent of the metadata file. */
	struct xfs_inode		*dp;

	/* Locks held on dp */
	unsigned int			dp_ilock_flags;

	/* Transaction block reservations */
	unsigned int			link_resblks;
	unsigned int			unlink_resblks;

	/* Parent pointer updates */
	struct xfs_parent_args		link_ppargs;
	struct xfs_parent_args		unlink_ppargs;
};

/* Release resources tracked in the buffer. */
STATIC void
xchk_metapath_cleanup(
	void			*buf)
{
	struct xchk_metapath	*mpath = buf;
	struct xfs_scrub	*sc = mpath->sc;

	if (mpath->dp) {
		if (mpath->dp_ilock_flags)
			xfs_iunlock(mpath->dp, mpath->dp_ilock_flags);
		xchk_irele(sc, mpath->dp);
	}
	if (mpath->path)
		xfs_imeta_free_path(mpath->path);
}

int
xchk_setup_metapath(
	struct xfs_scrub	*sc)
{
	struct xchk_metapath	*mpath;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_inode	*ip = NULL;
	struct xfs_rtgroup	*rtg;
	struct xfs_imeta_path	*path;
	int			error;

	if (!xfs_has_metadir(mp))
		return -ENOENT;
	if (sc->sm->sm_gen)
		return -EINVAL;

	switch (sc->sm->sm_ino) {
	case XFS_SCRUB_METAPATH_RTRMAPBT:
		/* empty */
		break;
	default:
		if (sc->sm->sm_agno)
			return -EINVAL;
		break;
	}

	mpath = kzalloc(sizeof(struct xchk_metapath), XCHK_GFP_FLAGS);
	if (!mpath)
		return -ENOMEM;
	mpath->sc = sc;
	sc->buf = mpath;
	sc->buf_cleanup = xchk_metapath_cleanup;

	/* Select the metadir path and the metadata file. */
	switch (sc->sm->sm_ino) {
	case XFS_SCRUB_METAPATH_RTBITMAP:
		mpath->path = &XFS_IMETA_RTBITMAP;
		ip = mp->m_rbmip;
		break;
	case XFS_SCRUB_METAPATH_RTSUMMARY:
		mpath->path = &XFS_IMETA_RTSUMMARY;
		ip = mp->m_rsumip;
		break;
	case XFS_SCRUB_METAPATH_USRQUOTA:
		mpath->path = &XFS_IMETA_USRQUOTA;
		if (XFS_IS_UQUOTA_ON(mp))
			ip = xfs_quota_inode(mp, XFS_DQTYPE_USER);
		break;
	case XFS_SCRUB_METAPATH_GRPQUOTA:
		mpath->path = &XFS_IMETA_GRPQUOTA;
		if (XFS_IS_GQUOTA_ON(mp))
			ip = xfs_quota_inode(mp, XFS_DQTYPE_GROUP);
		break;
	case XFS_SCRUB_METAPATH_PRJQUOTA:
		mpath->path = &XFS_IMETA_PRJQUOTA;
		if (XFS_IS_PQUOTA_ON(mp))
			ip = xfs_quota_inode(mp, XFS_DQTYPE_PROJ);
		break;
	case XFS_SCRUB_METAPATH_RTRMAPBT:
		error = xfs_rtrmapbt_create_path(mp, sc->sm->sm_agno, &path);
		if (error)
			return error;
		mpath->path = path;
		rtg = xfs_rtgroup_get(mp, sc->sm->sm_agno);
		if (rtg) {
			ip = rtg->rtg_rmapip;
			xfs_rtgroup_put(rtg);
		}
		break;
	default:
		return -EINVAL;
	}

	if (mpath->path->im_depth < 1) {
		/* Not supposed to have any zero-length paths */
		ASSERT(mpath->path->im_depth >= 1);
		return -EFSCORRUPTED;
	}

	if (!ip)
		return -ENOENT;

	error = xchk_install_live_inode(sc, ip);
	if (error)
		return error;

	mpath->xname.name = mpath->path->im_path[mpath->path->im_depth - 1];
	mpath->xname.len = strlen(mpath->xname.name);
	mpath->xname.type = xfs_mode_to_ftype(VFS_I(sc->ip)->i_mode);
	return 0;
}

/*
 * Try to attach the parent metadata directory to the scrub context.  Returns
 * true if a parent is attached.
 */
STATIC bool
xchk_metapath_try_attach_parent(
	struct xchk_metapath	*mpath)
{
	struct xfs_scrub	*sc = mpath->sc;

	if (mpath->dp)
		return true;

	/*
	 * Grab the parent we just ensured.  If the parent itself is corrupt
	 * enough that xfs_iget fails, someone else will have to fix it for us.
	 * That someone are the inode and scrubbers, invoked on the parent dir
	 * via handle.
	 */
	xfs_imeta_dir_parent(sc->tp, mpath->path, &mpath->dp);

	trace_xchk_metapath_try_attach_parent(sc, mpath->path, mpath->dp,
			NULLFSINO);
	return mpath->dp != NULL;
}

/*
 * Take the ILOCK on the metadata directory parent and child.  We do not know
 * that the metadata directory is not corrupt, so we lock the parent and try
 * to lock the child.  Returns 0 if successful, or -EINTR to abort the scrub.
 */
STATIC int
xchk_metapath_ilock_both(
	struct xchk_metapath	*mpath)
{
	struct xfs_scrub	*sc = mpath->sc;
	int			error = 0;

	while (true) {
		xfs_ilock(mpath->dp, XFS_ILOCK_EXCL);
		if (xchk_ilock_nowait(sc, XFS_ILOCK_EXCL)) {
			mpath->dp_ilock_flags |= XFS_ILOCK_EXCL;
			return 0;
		}
		xfs_iunlock(mpath->dp, XFS_ILOCK_EXCL);

		if (xchk_should_terminate(sc, &error))
			return error;

		delay(1);
	}

	ASSERT(0);
	return -EINTR;
}

/* Unlock parent and child inodes. */
static inline void
xchk_metapath_iunlock(
	struct xchk_metapath	*mpath)
{
	struct xfs_scrub	*sc = mpath->sc;

	xchk_iunlock(sc, XFS_ILOCK_EXCL);

	mpath->dp_ilock_flags &= ~XFS_ILOCK_EXCL;
	xfs_iunlock(mpath->dp, XFS_ILOCK_EXCL);
}

int
xchk_metapath(
	struct xfs_scrub	*sc)
{
	struct xchk_metapath	*mpath = sc->buf;
	xfs_ino_t		ino = NULLFSINO;
	int			error;

	error = xchk_trans_alloc_empty(sc);
	if (error)
		return error;

	/* Grab the parent; if we can't, it's corrupt. */
	if (!xchk_metapath_try_attach_parent(mpath)) {
		xchk_ino_set_corrupt(sc, sc->ip->i_ino);
		goto out_cancel;
	}

	error = xchk_metapath_ilock_both(mpath);
	if (error)
		goto out_cancel;

	/* Make sure the parent dir has a dirent pointing to this file. */
	error = xchk_dir_lookup(sc, mpath->dp, &mpath->xname, &ino);
	trace_xchk_metapath_lookup(sc, mpath->path, mpath->dp, ino);
	if (error == -ENOENT) {
		/* No directory entry at all */
		xchk_ino_set_corrupt(sc, sc->ip->i_ino);
		error = 0;
		goto out_ilock;
	}
	if (!xchk_fblock_xref_process_error(sc, XFS_DATA_FORK, 0, &error))
		goto out_ilock;
	if (ino != sc->ip->i_ino) {
		/* Pointing to wrong inode */
		xchk_ino_set_corrupt(sc, sc->ip->i_ino);
	}

out_ilock:
	xchk_metapath_iunlock(mpath);
out_cancel:
	xchk_trans_cancel(sc);
	return error;
}

#ifdef CONFIG_XFS_ONLINE_REPAIR
/* Create the dirent represented by the final component of the path. */
STATIC int
xrep_metapath_link(
	struct xchk_metapath	*mpath)
{
	struct xfs_scrub	*sc = mpath->sc;

	mpath->du.dp = mpath->dp;
	mpath->du.name = &mpath->xname;
	mpath->du.ip = sc->ip;

	if (xfs_has_parent(sc->mp))
		mpath->du.ppargs = &mpath->link_ppargs;
	else
		mpath->du.ppargs = NULL;

	trace_xrep_metapath_link(sc, mpath->path, mpath->dp, sc->ip->i_ino);

	return xfs_dir_add_child(sc->tp, mpath->link_resblks, &mpath->du);
}

/* Remove the dirent at the final component of the path. */
STATIC int
xrep_metapath_unlink(
	struct xchk_metapath	*mpath,
	xfs_ino_t		wrong_ino)
{
	struct xfs_scrub	*sc = mpath->sc;

	mpath->du.dp = mpath->dp;
	mpath->du.name = &mpath->xname;
	mpath->du.ip = sc->ip;

	if (xfs_has_parent(sc->mp))
		mpath->du.ppargs = &mpath->unlink_ppargs;
	else
		mpath->du.ppargs = NULL;

	trace_xrep_metapath_unlink(sc, mpath->path, mpath->dp, wrong_ino);

	return xfs_dir_add_child(sc->tp, mpath->unlink_resblks, &mpath->du);
}

/*
 * Make sure the metadata directory path points to the child being examined.
 *
 * Repair needs to be able to create a directory structure, create its own
 * transactions, and take ILOCKs.  This function /must/ be called after all
 * other repairs have completed.
 */
int
xrep_metapath(
	struct xfs_scrub	*sc)
{
	struct xchk_metapath	*mpath = sc->buf;
	struct xfs_mount	*mp = sc->mp;
	xfs_ino_t		ino = NULLFSINO;
	int			error = 0;

	/*
	 * Make sure the directory path exists all the way down to where the
	 * parent pointer should be.
	 */
	error = xfs_imeta_ensure_dirpath(sc->mp, mpath->path);
	if (error)
		return error;

	/* Make sure the parent is attached now. */
	error = xchk_trans_alloc_empty(sc);
	if (error)
		return error;
	if (!xchk_metapath_try_attach_parent(mpath)) {
		xchk_trans_cancel(sc);
		return -EFSCORRUPTED;
	}
	xchk_trans_cancel(sc);

	/*
	 * Make sure the child file actually has an attr fork to receive a new
	 * parent pointer if the fs has parent pointers.
	 */
	if (xfs_has_parent(mp)) {
		error = xfs_attr_add_fork(sc->ip,
				sizeof(struct xfs_attr_sf_hdr), 1);
		if (error)
			return error;
	}

	/* Compute block reservation required to unlink and link a file. */
	mpath->unlink_resblks = xfs_remove_space_res(mp, MAXNAMELEN);
	mpath->link_resblks = xfs_link_space_res(mp, MAXNAMELEN);

	/* Allocate parent pointer tracking. */
	xfs_parent_args_init(mp, &mpath->link_ppargs);
	xfs_parent_args_init(mp, &mpath->unlink_ppargs);

	/* Allocate transaction, lock inodes, join to transaction. */
	error = xchk_trans_alloc(sc, mpath->link_resblks +
				     mpath->unlink_resblks);
	if (error)
		return error;

	error = xchk_metapath_ilock_both(mpath);
	if (error) {
		xchk_trans_cancel(sc);
		return error;
	}
	xfs_trans_ijoin(sc->tp, mpath->dp, 0);
	xfs_trans_ijoin(sc->tp, sc->ip, 0);

	/* Figure out what to do about this file. */
	error = xchk_dir_lookup(sc, mpath->dp, &mpath->xname, &ino);
	trace_xrep_metapath_lookup(sc, mpath->path, mpath->dp, ino);
	if (error == -ENOENT) {
		/* Add new link */
		error = xrep_metapath_link(mpath);
		if (error)
			goto out_cancel;
		goto out_commit;
	}
	if (error)
		goto out_cancel;

	if (ino == sc->ip->i_ino)
		goto out_cancel;

	/* Remove the dirent at the end of the path and finish deferred ops. */
	error = xrep_metapath_unlink(mpath, ino);
	if (error)
		goto out_cancel;
	error = xrep_defer_finish(sc);
	if (error)
		goto out_cancel;

	/* Add new dirent link, having not unlocked the inodes, and commit. */
	error = xrep_metapath_link(mpath);
	if (error)
		goto out_cancel;

out_commit:
	error = xrep_trans_commit(sc);
	goto out_ilock;
out_cancel:
	xchk_trans_cancel(sc);
out_ilock:
	xchk_metapath_iunlock(mpath);
	return error;
}
#endif /* CONFIG_XFS_ONLINE_REPAIR */
