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
#include "xfs_health.h"
#include "xfs_errortag.h"
#include "xfs_error.h"
#include "xfs_btree.h"
#include "xfs_alloc.h"

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

/* Special path to locate the metadata directory tree root. */
const struct xfs_imeta_path XFS_IMETA_METADIR = {
	.im_depth = 0,
	.im_ftype = XFS_DIR3_FT_DIR,
};

/* Are these two paths equal? */
STATIC bool
xfs_imeta_path_compare(
	const struct xfs_imeta_path	*a,
	const struct xfs_imeta_path	*b)
{
	unsigned int			i;

	if (a == b)
		return true;

	if (a->im_depth != b->im_depth)
		return false;

	for (i = 0; i < a->im_depth; i++)
		if (a->im_path[i] != b->im_path[i] &&
		    strcmp(a->im_path[i], b->im_path[i]))
			return false;

	return true;
}

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
 * Is this metadata inode pointer ok?  We allow the fields to be set to
 * NULLFSINO if the metadata structure isn't present, and we don't allow
 * obviously incorrect inode pointers.
 */
static inline bool
xfs_imeta_verify(
	struct xfs_mount	*mp,
	xfs_ino_t		ino)
{
	if (ino == NULLFSINO)
		return true;
	return xfs_verify_ino(mp, ino);
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

	if (!S_ISDIR(VFS_I(dp)->i_mode)) {
		xfs_fs_mark_sick(dp->i_mount, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

	error = xfs_imeta_dir_lookup(tp, dp, xname, ino);
	if (error)
		return error;
	if (!xfs_verify_ino(dp->i_mount, *ino)) {
		xfs_fs_mark_sick(dp->i_mount, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}
	if (type_wanted != XFS_DIR3_FT_UNKNOWN && xname->type != type_wanted) {
		xfs_fs_mark_sick(dp->i_mount, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

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
	error = xfs_imeta_iget(tp, mp->m_metadirip->i_ino, XFS_DIR3_FT_DIR,
			&dp);
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
		error = xfs_imeta_iget(tp, ino, XFS_DIR3_FT_DIR, &ip);
		if (error)
			goto out_rele;

		xfs_iunlock(dp, XFS_ILOCK_EXCL);
		xfs_imeta_irele(dp);
		dp = ip;
	}

out:
	*dpp = dp;
	return 0;

out_rele:
	xfs_iunlock(dp, XFS_ILOCK_EXCL);
	xfs_imeta_irele(dp);
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
	struct xfs_mount		*mp = tp->t_mountp;
	int				error;

	ASSERT(xfs_imeta_path_check(path));

	/* metadir ino is recorded in superblock */
	if (xfs_imeta_path_compare(path, &XFS_IMETA_METADIR)) {
		*inop = mp->m_sb.sb_metadirino;
		return 0;
	}

	ASSERT(path->im_depth > 0);

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
	xfs_imeta_irele(dp);

	if (error)
		return error;

	if (!xfs_imeta_verify(mp, *inop)) {
		xfs_fs_mark_sick(mp, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

	return 0;
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

/*
 * Create a new metadata root directory.  The superblock field must not already
 * be pointing to an inode.  See xfs_imeta_create for calling conventions.
 */
int
xfs_imeta_create_root(
	struct xfs_imeta_update		*upd)
{
	struct xfs_icreate_args		args = {
		.mode			= S_IFDIR,
		.flags			= XFS_ICREATE_UNLINKABLE,
	};
	struct xfs_mount		*mp = upd->mp;
	struct xfs_buf			*bp;
	xfs_ino_t			ino;
	int				error;

	/* Reject if the sb already points to some inode. */
	if (mp->m_sb.sb_metadirino != NULLFSINO)
		return -EEXIST;

	/* Create a new inode and set the sb pointer. */
	error = xfs_dialloc(&upd->tp, NULL, args.mode, &ino);
	if (error)
		return error;
	error = xfs_icreate(upd->tp, ino, &args, &upd->ip);
	if (error)
		return error;
	upd->ip_locked = true;
	mp->m_sb.sb_metadirino = ino;

	/*
	 * Update the inode flags in the ondisk superblock without touching
	 * the summary counters.  We have not quiesced inode chunk allocation,
	 * so we cannot coordinate with updates to the icount and ifree percpu
	 * counters.
	 */
	bp = xfs_trans_getsb(upd->tp);
	xfs_sb_to_disk(bp->b_addr, &mp->m_sb);
	xfs_trans_buf_set_type(upd->tp, bp, XFS_BLFT_SB_BUF);
	xfs_trans_log_buf(upd->tp, bp, 0, sizeof(struct xfs_dsb) - 1);

	/* Initialize the root directory. */
	xfs_imeta_set_iflag(upd->tp, upd->ip);
	return xfs_dir_init(upd->tp, upd->ip, upd->ip);
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
	if (xfs_imeta_path_compare(upd->path, &XFS_IMETA_METADIR)) {
		ASSERT(0);
		xfs_fs_mark_sick(mp, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

	ASSERT(upd->path->im_depth > 0);

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
	error = xfs_dialloc(&upd->tp, upd->dp, mode, &ino);
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
	if (xfs_imeta_path_compare(upd->path, &XFS_IMETA_METADIR)) {
		ASSERT(0);
		xfs_fs_mark_sick(mp, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

	ASSERT(upd->path->im_depth > 0);

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

	if (path->im_flags & XFS_IMETA_PATH_STATIC)
		return;

	for (i = 0; i < path->im_depth; i++) {
		if ((path->im_dynamicmask & (1ULL << i)) && path->im_path[i])
			kfree(path->im_path[i]);
	}
	kfree(path->im_path);
	kfree(path);
}

/*
 * Is the amount of space that could be allocated towards a given metadata
 * file at or beneath a certain threshold?
 */
static inline bool
xfs_imeta_resv_can_cover(
	struct xfs_inode	*ip,
	int64_t			rhs)
{
	/*
	 * The amount of space that can be allocated to this metadata file is
	 * the remaining reservation for the particular metadata file + the
	 * global free block count.  Take care of the first case to avoid
	 * touching the per-cpu counter.
	 */
	if (ip->i_delayed_blks >= rhs)
		return true;

	/*
	 * There aren't enough blocks left in the inode's reservation, but it
	 * isn't critical unless there also isn't enough free space.
	 */
	return __percpu_counter_compare(&ip->i_mount->m_fdblocks,
			rhs - ip->i_delayed_blks, 2048) >= 0;
}

/*
 * Is this metadata file critically low on blocks?  For now we'll define that
 * as the number of blocks we can get our hands on being less than 10% of what
 * we reserved or less than some arbitrary number (maximum btree height).
 */
bool
xfs_imeta_resv_critical(
	struct xfs_inode	*ip)
{
	uint64_t		asked_low_water;

	if (!ip)
		return false;

	ASSERT(xfs_is_metadir_inode(ip));
	trace_xfs_imeta_resv_critical(ip, 0);

	if (!xfs_imeta_resv_can_cover(ip, ip->i_mount->m_rtbtree_maxlevels))
		return true;

	asked_low_water = div_u64(ip->i_meta_resv_asked, 10);
	if (!xfs_imeta_resv_can_cover(ip, asked_low_water))
		return true;

	return XFS_TEST_ERROR(false, ip->i_mount,
			XFS_ERRTAG_IMETA_RESV_CRITICAL);
}

/* Allocate a block from the metadata file's reservation. */
void
xfs_imeta_resv_alloc_extent(
	struct xfs_inode	*ip,
	struct xfs_alloc_arg	*args)
{
	int64_t			len = args->len;

	ASSERT(xfs_is_metadir_inode(ip));
	ASSERT(XFS_IS_DQDETACHED(ip->i_mount, ip));
	ASSERT(args->resv == XFS_AG_RESV_IMETA);

	trace_xfs_imeta_resv_alloc_extent(ip, args->len);

	/*
	 * Allocate the blocks from the metadata inode's block reservation
	 * and update the ondisk sb counter.
	 */
	if (ip->i_delayed_blks > 0) {
		int64_t		from_resv;

		from_resv = min_t(int64_t, len, ip->i_delayed_blks);
		ip->i_delayed_blks -= from_resv;
		xfs_mod_delalloc(ip, 0, -from_resv);
		xfs_trans_mod_sb(args->tp, XFS_TRANS_SB_RES_FDBLOCKS,
				-from_resv);
		len -= from_resv;
	}

	/*
	 * Any allocation in excess of the reservation requires in-core and
	 * on-disk fdblocks updates.  If we can grab @len blocks from the
	 * in-core fdblocks then all we need to do is update the on-disk
	 * superblock; if not, then try to steal some from the transaction's
	 * block reservation.  Overruns are only expected for rmap btrees.
	 */
	if (len) {
		unsigned int	field;
		int		error;

		error = xfs_dec_fdblocks(ip->i_mount, len, true);
		if (error)
			field = XFS_TRANS_SB_FDBLOCKS;
		else
			field = XFS_TRANS_SB_RES_FDBLOCKS;

		xfs_trans_mod_sb(args->tp, field, -len);
	}

	ip->i_nblocks += args->len;
	xfs_trans_log_inode(args->tp, ip, XFS_ILOG_CORE);
}

/* Free a block to the metadata file's reservation. */
void
xfs_imeta_resv_free_extent(
	struct xfs_inode	*ip,
	struct xfs_trans	*tp,
	xfs_filblks_t		len)
{
	int64_t			to_resv;

	ASSERT(xfs_is_metadir_inode(ip));
	ASSERT(XFS_IS_DQDETACHED(ip->i_mount, ip));
	trace_xfs_imeta_resv_free_extent(ip, len);

	ip->i_nblocks -= len;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);

	/*
	 * Add the freed blocks back into the inode's delalloc reservation
	 * until it reaches the maximum size.  Update the ondisk fdblocks only.
	 */
	to_resv = ip->i_meta_resv_asked - (ip->i_nblocks + ip->i_delayed_blks);
	if (to_resv > 0) {
		to_resv = min_t(int64_t, to_resv, len);
		ip->i_delayed_blks += to_resv;
		xfs_mod_delalloc(ip, 0, to_resv);
		xfs_trans_mod_sb(tp, XFS_TRANS_SB_RES_FDBLOCKS, to_resv);
		len -= to_resv;
	}

	/*
	 * Everything else goes back to the filesystem, so update the in-core
	 * and on-disk counters.
	 */
	if (len)
		xfs_trans_mod_sb(tp, XFS_TRANS_SB_FDBLOCKS, len);
}

/* Release a metadata file's space reservation. */
void
xfs_imeta_resv_free_inode(
	struct xfs_inode	*ip)
{
	if (!ip)
		return;

	ASSERT(xfs_is_metadir_inode(ip));
	trace_xfs_imeta_resv_free(ip, 0);

	xfs_mod_delalloc(ip, 0, -ip->i_delayed_blks);
	xfs_add_fdblocks(ip->i_mount, ip->i_delayed_blks);
	ip->i_delayed_blks = 0;
	ip->i_meta_resv_asked = 0;
}

/* Set up a metadata file's space reservation. */
int
xfs_imeta_resv_init_inode(
	struct xfs_inode	*ip,
	xfs_filblks_t		ask)
{
	xfs_filblks_t		hidden_space;
	xfs_filblks_t		used;
	int			error;

	if (!ip || ip->i_meta_resv_asked > 0)
		return 0;

	ASSERT(xfs_is_metadir_inode(ip));

	/*
	 * Space taken by all other metadata btrees are accounted on-disk as
	 * used space.  We therefore only hide the space that is reserved but
	 * not used by the trees.
	 */
	used = ip->i_nblocks;
	if (used > ask)
		ask = used;
	hidden_space = ask - used;

	error = xfs_dec_fdblocks(ip->i_mount, hidden_space, true);
	if (error) {
		trace_xfs_imeta_resv_init_error(ip, error, _RET_IP_);
		return error;
	}

	xfs_mod_delalloc(ip, 0, hidden_space);
	ip->i_delayed_blks = hidden_space;
	ip->i_meta_resv_asked = ask;

	trace_xfs_imeta_resv_init(ip, ask);
	return 0;
}
