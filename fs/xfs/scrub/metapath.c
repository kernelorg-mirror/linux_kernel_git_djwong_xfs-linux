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
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/readdir.h"

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

	/* Path for this metadata file */
	const struct xfs_imeta_path	*path;

	/* Directory parent of the metadata file. */
	struct xfs_inode		*dp;

	/* Locks held on dp */
	unsigned int			dp_ilock_flags;
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
	int			error;

	if (!xfs_has_metadir(mp))
		return -ENOENT;
	if (sc->sm->sm_gen || sc->sm->sm_agno)
		return -EINVAL;

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
