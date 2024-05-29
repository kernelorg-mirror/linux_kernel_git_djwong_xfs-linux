// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_trans.h"
#include "xfs_imeta.h"
#include "xfs_trace.h"
#include "xfs_inode.h"
#include "xfs_quota.h"
#include "xfs_ialloc.h"
#include "xfs_bmap_btree.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_trans_space.h"
#include "xfs_ag.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "xfs_parent.h"

/*
 * Metadata Directory Tree
 * =======================
 *
 * These functions provide an abstraction layer for looking up, creating, and
 * deleting metadata inodes that live within a special metadata directory tree.
 *
 * This code does not manage the five existing metadata inodes: real time
 * bitmap & summary; and the user, group, and quotas.  All other metadata
 * inodes must use only the xfs_imeta_* functions.
 *
 * Callers wishing to create or hardlink a metadata inode must create an
 * xfs_imeta_update structure, call the appropriate xfs_imeta function, and
 * then call xfs_imeta_commit or xfs_imeta_cancel to commit or cancel the
 * update.  Files in the metadata directory tree currently cannot be unlinked.
 *
 * When the metadir feature is enabled, all metadata inodes must have the
 * "metadata" inode flag set to prevent them from being exposed to the outside
 * world.
 *
 * Callers must take the ILOCK of any inode in the metadata directory tree to
 * synchronize access to that inode.  It is never necessary to take the IOLOCK
 * or the MMAPLOCK since metadata inodes should never be exposed to user space.
 */

/* Is this path ok? */
static inline bool
xfs_imeta_path_check(
	const struct xfs_imeta_path	*path)
{
	return path->im_depth <= XFS_IMETA_MAX_DEPTH;
}

static inline void
xfs_imeta_set_xname(
	struct xfs_name			*xname,
	const struct xfs_imeta_path	*path,
	unsigned int			path_idx,
	unsigned char			ftype)
{
	xname->name = (const unsigned char *)path->im_path[path_idx];
	xname->len = strlen(path->im_path[path_idx]);
	xname->type = ftype;
}

/*
 * Look up the inode number and filetype for an exact name in a directory.
 * Caller must hold ILOCK_EXCL.
 */
static inline int
xfs_imeta_dir_lookup(
	struct xfs_trans	*tp,
	struct xfs_inode	*dp,
	struct xfs_name		*xname,
	xfs_ino_t		*ino)
{
	struct xfs_da_args	args = {
		.trans		= tp,
		.dp		= dp,
		.geo		= dp->i_mount->m_dir_geo,
		.name		= xname->name,
		.namelen	= xname->len,
		.hashval	= xfs_dir2_hashname(dp->i_mount, xname),
		.whichfork	= XFS_DATA_FORK,
		.op_flags	= XFS_DA_OP_OKNOENT,
		.owner		= dp->i_ino,
	};
	int			error;

	if (xfs_is_shutdown(dp->i_mount))
		return -EIO;

	error = xfs_dir_lookup_args(&args);
	if (error)
		return error;

	*ino = args.inumber;
	xname->type = args.filetype;
	return 0;
}

/*
 * Given a parent directory @dp and a metadata inode path component @xname,
 * Look up the inode number in the directory, returning it in @ino.
 * @xname.type must match the directory entry's ftype.
 *
 * Caller must hold ILOCK_EXCL.
 */
static inline int
xfs_imeta_lookup_component(
	struct xfs_trans	*tp,
	struct xfs_inode	*dp,
	struct xfs_name		*xname,
	xfs_ino_t		*ino)
{
	int			type_wanted = xname->type;
	int			error;

	if (!S_ISDIR(VFS_I(dp)->i_mode))
		return -EFSCORRUPTED;

	error = xfs_imeta_dir_lookup(tp, dp, xname, ino);
	if (error)
		return error;
	if (!xfs_verify_ino(dp->i_mount, *ino))
		return -EFSCORRUPTED;
	if (type_wanted != XFS_DIR3_FT_UNKNOWN && xname->type != type_wanted)
		return -EFSCORRUPTED;

	trace_xfs_imeta_lookup_component(dp, xname, *ino);
	return 0;
}

/*
 * Traverse a metadata directory tree path, returning the inode corresponding
 * to the parent of the last path component.  If any of the path components do
 * not exist, return -ENOENT.  Caller must supply a transaction to avoid
 * livelocks on btree cycles.
 *
 * @dp is returned without any locks held.
 */
int
xfs_imeta_iget_parent(
	struct xfs_trans		*tp,
	const struct xfs_imeta_path	*path,
	struct xfs_inode		**dpp)
{
	struct xfs_name			xname;
	struct xfs_mount		*mp = tp->t_mountp;
	struct xfs_inode		*dp = NULL;
	xfs_ino_t			ino;
	unsigned int			i;
	int				error;

	/* Caller wanted the root, we're done! */
	if (path->im_depth == 0)
		goto out;

	/* No metadata directory means no parent. */
	if (mp->m_metadirip == NULL)
		return -ENOENT;

	/* Grab a new reference to the metadir root dir. */
	error = xfs_imeta_iget(tp, mp->m_metadirip->i_ino, S_IFDIR, &dp);
	if (error)
		return error;

	for (i = 0; i < path->im_depth - 1; i++) {
		struct xfs_inode	*ip = NULL;

		xfs_ilock(dp, XFS_ILOCK_EXCL);

		/* Look up the name in the current directory. */
		xfs_imeta_set_xname(&xname, path, i, XFS_DIR3_FT_DIR);
		error = xfs_imeta_lookup_component(tp, dp, &xname, &ino);
		if (error)
			goto out_rele;

		/*
		 * Grab the child inode while we still have the parent
		 * directory locked.
		 */
		error = xfs_imeta_iget(tp, ino, S_IFDIR, &ip);
		if (error)
			goto out_rele;

		xfs_iunlock(dp, XFS_ILOCK_EXCL);
		xfs_irele(dp);
		dp = ip;
	}

out:
	*dpp = dp;
	return 0;

out_rele:
	xfs_iunlock(dp, XFS_ILOCK_EXCL);
	xfs_irele(dp);
	return error;
}

/*
 * Look up a metadata inode from the metadata directory.  If the last path
 * component doesn't exist, return NULLFSINO.  If any other part of the path
 * does not exist, return -ENOENT so we can distinguish the two.
 */
int
xfs_imeta_lookup(
	struct xfs_trans		*tp,
	const struct xfs_imeta_path	*path,
	xfs_ino_t			*inop)
{
	struct xfs_name			xname;
	struct xfs_inode		*dp = NULL;
	int				error;

	ASSERT(xfs_imeta_path_check(path));

	/* Metadata directory root cannot be used with this function. */
	if (path->im_depth == 0) {
		ASSERT(0);
		return -EFSCORRUPTED;
	}

	/* Find the parent of the last path component. */
	error = xfs_imeta_iget_parent(tp, path, &dp);
	if (error)
		return error;

	xfs_ilock(dp, XFS_ILOCK_EXCL);

	/* Look up the name in the current directory. */
	xfs_imeta_set_xname(&xname, path, path->im_depth - 1, path->im_ftype);
	error = xfs_imeta_lookup_component(tp, dp, &xname, inop);
	if (error == -ENOENT) {
		*inop = NULLFSINO;
		error = 0;
	}

	xfs_iunlock(dp, XFS_ILOCK_EXCL);
	xfs_irele(dp);

	return error;
}

/* Set up an inode to be recognized as a metadata directory inode. */
void
xfs_imeta_set_iflag(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	VFS_I(ip)->i_mode &= ~0777;
	VFS_I(ip)->i_uid = GLOBAL_ROOT_UID;
	VFS_I(ip)->i_gid = GLOBAL_ROOT_GID;
	ip->i_projid = 0;
	ip->i_diflags |= (XFS_DIFLAG_IMMUTABLE | XFS_DIFLAG_SYNC |
			  XFS_DIFLAG_NOATIME | XFS_DIFLAG_NODUMP |
			  XFS_DIFLAG_NODEFRAG);
	if (S_ISDIR(VFS_I(ip)->i_mode))
		ip->i_diflags |= XFS_DIFLAG_NOSYMLINKS;
	ip->i_diflags2 &= ~XFS_DIFLAG2_DAX;
	ip->i_diflags2 |= XFS_DIFLAG2_METADIR;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
}


/* Clear the metadata directory inode flag. */
void
xfs_imeta_clear_iflag(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	ASSERT(xfs_is_metadir_inode(ip));
	ASSERT(VFS_I(ip)->i_nlink == 0);

	ip->i_diflags2 &= ~XFS_DIFLAG2_METADIR;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
}

/* Initialize a metadata update structure. */
static inline int
xfs_imeta_init_update(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path,
	struct xfs_imeta_update		*upd)
{
	struct xfs_trans		*tp;
	int				error;

	ASSERT(xfs_has_metadir(mp));

	memset(upd, 0, sizeof(struct xfs_imeta_update));
	upd->mp = mp;
	upd->path = path;

	/*
	 * Find the parent of the last path component.  If the parent path does
	 * not exist, we consider this corruption because paths are supposed
	 * to exist.  For example, if the path is /quota/user, we require that
	 * /quota already exists.
	 */
	error = xfs_trans_alloc_empty(mp, &tp);
	if (error)
		return error;
	error = xfs_imeta_iget_parent(tp, upd->path, &upd->dp);
	xfs_trans_cancel(tp);
	if (error == -ENOENT)
		return -EFSCORRUPTED;
	if (error)
		return error;

	return xfs_parent_start(mp, &upd->ppargs);
}

/*
 * Unlock and release resources after committing (or cancelling) a metadata
 * directory tree operation.  The caller retains its reference to @upd->ip
 * and must release it explicitly.
 */
static inline void
xfs_imeta_teardown(
	struct xfs_imeta_update		*upd,
	int				error)
{
	trace_xfs_imeta_teardown(upd, error);

	if (upd->ppargs) {
		xfs_parent_finish(upd->mp, upd->ppargs);
		upd->ppargs = NULL;
	}

	if (upd->ip) {
		if (upd->ip_locked)
			xfs_iunlock(upd->ip, XFS_ILOCK_EXCL);
		upd->ip_locked = false;
	}

	if (upd->dp) {
		if (upd->dp_locked)
			xfs_iunlock(upd->dp, XFS_ILOCK_EXCL);
		upd->dp_locked = false;

		xfs_irele(upd->dp);
		upd->dp = NULL;
	}
}

/*
 * Begin the process of creating a metadata file by allocating transactions
 * and taking whatever resources we're going to need.
 */
int
xfs_imeta_start_create(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path,
	struct xfs_imeta_update		*upd)
{
	int				error;

	error = xfs_imeta_init_update(mp, path, upd);
	if (error)
		return error;

	/*
	 * If we ever need the ability to create rt metadata files on a
	 * pre-metadir filesystem, we'll need to dqattach the parent here.
	 * Currently we assume that mkfs will create the files and quotacheck
	 * will account for them.
	 */

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_create,
			xfs_create_space_res(mp, MAXNAMELEN), 0, 0, &upd->tp);
	if (error)
		goto out_teardown;

	/*
	 * Lock the parent directory if there is one.  We can't ijoin it to
	 * the transaction until after the child file has been created.
	 */
	xfs_ilock(upd->dp, XFS_ILOCK_EXCL | XFS_ILOCK_PARENT);
	upd->dp_locked = true;

	trace_xfs_imeta_start_create(upd);
	return 0;
out_teardown:
	xfs_imeta_teardown(upd, error);
	return error;
}

/*
 * Create a metadata inode with the given @mode, and insert it into the
 * metadata directory tree at the given @upd->path.  The path up to the final
 * component must already exist.  The final path component must not exist.
 *
 * The new metadata inode will be attached to the update structure @upd->ip,
 * with the ILOCK held until the caller releases it.
 *
 * NOTE: This function may return a new inode to the caller even if it returns
 * a negative error code.  If an inode is passed back, the caller must finish
 * setting up the inode before releasing it.
 */
int
xfs_imeta_create(
	struct xfs_imeta_update		*upd,
	umode_t				mode)
{
	struct xfs_icreate_args		args = {
		.pip			= upd->dp,
		.mode			= mode,
	};
	struct xfs_name			xname;
	struct xfs_dir_update		du = {
		.dp			= upd->dp,
		.name			= &xname,
		.ppargs			= upd->ppargs,
	};
	struct xfs_mount		*mp = upd->mp;
	xfs_ino_t			ino;
	unsigned int			resblks;
	int				error;

	ASSERT(xfs_imeta_path_check(upd->path));
	xfs_assert_ilocked(upd->dp, XFS_ILOCK_EXCL);

	/* Metadata directory root cannot be created with this function. */
	if (upd->path->im_depth == 0) {
		ASSERT(0);
		return -EFSCORRUPTED;
	}

	/* Check that the name does not already exist in the directory. */
	xfs_imeta_set_xname(&xname, upd->path, upd->path->im_depth - 1,
			XFS_DIR3_FT_UNKNOWN);
	error = xfs_imeta_lookup_component(upd->tp, upd->dp, &xname, &ino);
	switch (error) {
	case -ENOENT:
		break;
	case 0:
		error = -EEXIST;
		fallthrough;
	default:
		return error;
	}

	/*
	 * A newly created regular or special file just has one directory
	 * entry pointing to them, but a directory also the "." entry
	 * pointing to itself.
	 */
	error = xfs_dialloc(&upd->tp, &args, &ino);
	if (error)
		return error;
	error = xfs_icreate(upd->tp, ino, &args, &upd->ip);
	if (error)
		return error;
	du.ip = upd->ip;
	xfs_imeta_set_iflag(upd->tp, upd->ip);
	upd->ip_locked = true;

	/*
	 * Join the directory inode to the transaction.  We do not do it
	 * earlier because xfs_dialloc rolls the transaction.
	 */
	xfs_trans_ijoin(upd->tp, upd->dp, 0);

	/* Create the entry. */
	if (S_ISDIR(args.mode))
		resblks = xfs_mkdir_space_res(mp, xname.len);
	else
		resblks = xfs_create_space_res(mp, xname.len);
	xname.type = xfs_mode_to_ftype(args.mode);

	trace_xfs_imeta_try_create(upd);

	error = xfs_dir_create_child(upd->tp, resblks, &du);
	if (error)
		return error;

	/* Metadir files are not accounted to quota. */

	trace_xfs_imeta_create(upd);

	return 0;
}

/*
 * Begin the process of linking a metadata file by allocating transactions
 * and locking whatever resources we're going to need.
 */
int
xfs_imeta_start_link(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path,
	struct xfs_inode		*ip,
	struct xfs_imeta_update		*upd)
{
	unsigned int			resblks;
	int				nospace_error = 0;
	int				error;

	error = xfs_imeta_init_update(mp, path, upd);
	if (error)
		return error;

	ASSERT(upd->dp != NULL);

	upd->ip = ip;

	resblks = xfs_link_space_res(mp, MAXNAMELEN);
	error = xfs_trans_alloc_dir(upd->dp, &M_RES(mp)->tr_link, upd->ip,
			&resblks, &upd->tp, &nospace_error);
	if (error)
		goto out_teardown;
	if (!resblks) {
		/* We don't allow reservationless updates. */
		xfs_trans_cancel(upd->tp);
		upd->tp = NULL;
		xfs_iunlock(upd->dp, XFS_ILOCK_EXCL);
		xfs_iunlock(upd->ip, XFS_ILOCK_EXCL);
		error = nospace_error;
		goto out_teardown;
	}

	upd->dp_locked = true;
	upd->ip_locked = true;

	trace_xfs_imeta_start_link(upd);
	return 0;
out_teardown:
	xfs_imeta_teardown(upd, error);
	return error;
}

/*
 * Link the metadata directory given by @path to the inode @upd->ip.
 * The path (up to the final component) must already exist, but the final
 * component must not already exist.
 */
int
xfs_imeta_link(
	struct xfs_imeta_update		*upd)
{
	struct xfs_name			xname;
	struct xfs_dir_update		du = {
		.dp			= upd->dp,
		.name			= &xname,
		.ip			= upd->ip,
		.ppargs			= upd->ppargs,
	};
	struct xfs_mount		*mp = upd->mp;
	xfs_ino_t			ino;
	unsigned int			resblks;
	int				error;

	ASSERT(xfs_imeta_path_check(upd->path));

	xfs_assert_ilocked(upd->dp, XFS_ILOCK_EXCL);
	xfs_assert_ilocked(upd->ip, XFS_ILOCK_EXCL);

	/* Metadata directory root cannot be linked. */
	if (upd->path->im_depth == 0) {
		ASSERT(0);
		return -EFSCORRUPTED;
	}

	/* Look up the name in the current directory. */
	xfs_imeta_set_xname(&xname, upd->path, upd->path->im_depth - 1,
			xfs_mode_to_ftype(VFS_I(upd->ip)->i_mode));
	error = xfs_imeta_lookup_component(upd->tp, upd->dp, &xname, &ino);
	switch (error) {
	case -ENOENT:
		break;
	case 0:
		error = -EEXIST;
		fallthrough;
	default:
		return error;
	}

	resblks = xfs_link_space_res(mp, xname.len);
	error = xfs_dir_add_child(upd->tp, resblks, &du);
	if (error)
		return error;

	trace_xfs_imeta_link(upd);

	return 0;
}

/* Commit a metadir update and unlock/drop all resources. */
int
xfs_imeta_commit(
	struct xfs_imeta_update		*upd)
{
	int				error;

	trace_xfs_imeta_commit(upd);

	error = xfs_trans_commit(upd->tp);
	upd->tp = NULL;

	xfs_imeta_teardown(upd, error);
	return error;
}

/* Cancel a metadir update and unlock/drop all resources. */
void
xfs_imeta_cancel(
	struct xfs_imeta_update		*upd,
	int				error)
{
	trace_xfs_imeta_cancel(upd);

	xfs_trans_cancel(upd->tp);
	upd->tp = NULL;

	xfs_imeta_teardown(upd, error);
}

/* Create a metadata for the last component of the path. */
STATIC int
xfs_imeta_mkdir(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path)
{
	struct xfs_imeta_update		upd = { };
	int				error;

	if (xfs_is_shutdown(mp))
		return -EIO;

	/* Allocate a transaction to create the last directory. */
	error = xfs_imeta_start_create(mp, path, &upd);
	if (error)
		return error;

	/* Create the subdirectory and take our reference. */
	error = xfs_imeta_create(&upd, S_IFDIR);
	if (error)
		goto out_cancel;

	error = xfs_imeta_commit(&upd);

	/*
	 * We don't pass the directory we just created to the caller, so finish
	 * setting up the inode, then release the dir and the dquots.
	 */
	goto out_irele;

out_cancel:
	xfs_imeta_cancel(&upd, error);
out_irele:
	/* Have to finish setting up the inode to ensure it's deleted. */
	if (upd.ip) {
		xfs_finish_inode_setup(upd.ip);
		xfs_irele(upd.ip);
	}
	return error;
}

/*
 * Make sure that every metadata directory path component exists and is a
 * directory.
 */
int
xfs_imeta_ensure_dirpath(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path)
{
	struct xfs_imeta_path		temp_path = {
		.im_path		= path->im_path,
		.im_ftype		= XFS_DIR3_FT_DIR,
	};
	int				error = 0;

	if (!xfs_has_metadir(mp))
		return 0;

	for (temp_path.im_depth = 1;
	     temp_path.im_depth < path->im_depth;
	     temp_path.im_depth++) {
		error = xfs_imeta_mkdir(mp, &temp_path);
		if (error && error != -EEXIST)
			return error;
	}

	return 0;
}

/* Create a path to a file within the metadata directory tree. */
int
xfs_imeta_create_file_path(
	struct xfs_mount	*mp,
	unsigned int		nr_components,
	struct xfs_imeta_path	**pathp)
{
	struct xfs_imeta_path	*p;
	const char		**components;

	p = kzalloc(sizeof(struct xfs_imeta_path), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	components = kvcalloc(nr_components, sizeof(char *), GFP_KERNEL);
	if (!components) {
		kfree(p);
		return -ENOMEM;
	}

	p->im_depth = nr_components;
	p->im_path = components;
	p->im_ftype = XFS_DIR3_FT_REG_FILE;
	*pathp = p;
	return 0;
}

/* Free a metadata directory tree path. */
void
xfs_imeta_free_path(
	const struct xfs_imeta_path	*path)
{
	unsigned int			i;

	for (i = 0; i < path->im_depth; i++) {
		if ((path->im_dynamicmask & (1ULL << i)) && path->im_path[i])
			kfree(path->im_path[i]);
	}
	kfree(path->im_path);
	kfree(path);
}
