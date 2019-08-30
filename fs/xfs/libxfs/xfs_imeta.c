// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
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
#include "xfs_bmap_btree.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_trans_space.h"

/*
 * Metadata Inode Number Management
 * ================================
 *
 * These functions provide an abstraction layer for looking up, creating, and
 * deleting metadata inodes.  These pointers live in the in-core superblock,
 * so the functions moderate access to those fields and take care of logging.
 *
 * For the five existing metadata inodes (real time bitmap & summary; and the
 * user, group, and quotas) we'll continue to maintain the in-core superblock
 * inodes for reads and only require xfs_imeta_create and xfs_imeta_unlink to
 * persist changes.  New metadata inode types must only use the xfs_imeta_*
 * functions.
 *
 * Callers wishing to create or unlink a metadata inode must pass in a
 * xfs_imeta_end structure.  After committing or cancelling the transaction,
 * this structure must be passed to xfs_imeta_end_update to free resources that
 * cannot be freed during the transaction.
 *
 * Right now we only support callers passing in the predefined metadata inode
 * paths; the goal is that callers will some day locate metadata inodes based
 * on a metadata inode directory structure.
 */

/* Static metadata inode paths */

const struct xfs_imeta_path XFS_IMETA_RTBITMAP = {
	.bogus = 0,
};

const struct xfs_imeta_path XFS_IMETA_RTSUMMARY = {
	.bogus = 1,
};

const struct xfs_imeta_path XFS_IMETA_USRQUOTA = {
	.bogus = 2,
};

const struct xfs_imeta_path XFS_IMETA_GRPQUOTA = {
	.bogus = 3,
};

const struct xfs_imeta_path XFS_IMETA_PRJQUOTA = {
	.bogus = 4,
};

/* Are these two paths equal? */
STATIC bool
xfs_imeta_path_compare(
	const struct xfs_imeta_path	*a,
	const struct xfs_imeta_path	*b)
{
	return a == b;
}

/* Is this path ok? */
static inline bool
xfs_imeta_path_check(
	const struct xfs_imeta_path	*path)
{
	return true;
}

/* Functions for storing and retrieving superblock inode values. */

/* Mapping of metadata inode paths to in-core superblock values. */
static const struct xfs_imeta_sbmap {
	const struct xfs_imeta_path	*path;
	unsigned int			offset;
} xfs_imeta_sbmaps[] = {
	{
		.path	= &XFS_IMETA_RTBITMAP,
		.offset	= offsetof(struct xfs_sb, sb_rbmino),
	},
	{
		.path	= &XFS_IMETA_RTSUMMARY,
		.offset	= offsetof(struct xfs_sb, sb_rsumino),
	},
	{
		.path	= &XFS_IMETA_USRQUOTA,
		.offset	= offsetof(struct xfs_sb, sb_uquotino),
	},
	{
		.path	= &XFS_IMETA_GRPQUOTA,
		.offset	= offsetof(struct xfs_sb, sb_gquotino),
	},
	{
		.path	= &XFS_IMETA_PRJQUOTA,
		.offset	= offsetof(struct xfs_sb, sb_pquotino),
	},
	{ NULL, 0 },
};

/* Return a pointer to the in-core superblock inode value. */
static inline xfs_ino_t *
xfs_imeta_sbmap_to_inop(
	struct xfs_mount		*mp,
	const struct xfs_imeta_sbmap	*map)
{
	return (xfs_ino_t *)(((char *)&mp->m_sb) + map->offset);
}

/* Compute location of metadata inode pointer in the in-core superblock */
static inline xfs_ino_t *
xfs_imeta_path_to_sb_inop(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path)
{
	const struct xfs_imeta_sbmap	*p;

	for (p = xfs_imeta_sbmaps; p->path; p++)
		if (xfs_imeta_path_compare(p->path, path))
			return xfs_imeta_sbmap_to_inop(mp, p);

	return NULL;
}

/* Look up a superblock metadata inode by its path. */
STATIC int
xfs_imeta_sb_lookup(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path,
	xfs_ino_t			*inop)
{
	xfs_ino_t			*sb_inop;

	sb_inop = xfs_imeta_path_to_sb_inop(mp, path);
	if (!sb_inop)
		return -EINVAL;

	trace_xfs_imeta_sb_lookup(mp, sb_inop);
	*inop = *sb_inop;
	return 0;
}

/*
 * Create a new metadata inode and set a superblock pointer to this new inode.
 * The superblock field must not already be pointing to an inode.
 */
STATIC int
xfs_imeta_sb_create(
	struct xfs_trans		**tpp,
	const struct xfs_imeta_path	*path,
	umode_t				mode,
	struct xfs_inode		**ipp)
{
	struct xfs_ialloc_args args = {
		.nlink			= S_ISDIR(mode) ? 2 : 1,
		.mode			= mode,
	};
	struct xfs_mount		*mp = (*tpp)->t_mountp;
	xfs_ino_t			*sb_inop;
	int				error;

	/* Reject if the sb already points to some inode. */
	sb_inop = xfs_imeta_path_to_sb_inop(mp, path);
	if (!sb_inop)
		return -EINVAL;

	if (*sb_inop != NULLFSINO)
		return -EEXIST;

	/* Otherwise, create the inode and set the sb pointer. */
	error = xfs_dir_ialloc(tpp, &args, ipp);
	if (error)
		return error;


	*sb_inop = (*ipp)->i_ino;
	trace_xfs_imeta_sb_create(mp, sb_inop);
	xfs_log_sb(*tpp);
	return 0;
}

/*
 * Clear the given inode pointer from the superblock and drop the link count
 * of the metadata inode.
 */
STATIC int
xfs_imeta_sb_unlink(
	struct xfs_trans		**tpp,
	const struct xfs_imeta_path	*path,
	struct xfs_inode		*ip)
{
	struct xfs_mount		*mp = (*tpp)->t_mountp;
	xfs_ino_t			*sb_inop;

	sb_inop = xfs_imeta_path_to_sb_inop(mp, path);
	if (!sb_inop)
		return -EINVAL;

	/* Reject if the sb doesn't point to the inode that was passed in. */
	if (*sb_inop != ip->i_ino)
		return -ENOENT;

	*sb_inop = NULLFSINO;
	trace_xfs_imeta_sb_unlink(mp, sb_inop);
	xfs_log_sb(*tpp);
	return xfs_droplink(*tpp, ip);
}

/* Set the given inode pointer to NULL in the superblock. */
STATIC int
xfs_imeta_sb_zap(
	struct xfs_trans		**tpp,
	const struct xfs_imeta_path	*path)
{
	struct xfs_mount		*mp = (*tpp)->t_mountp;
	xfs_ino_t			*sb_inop;

	sb_inop = xfs_imeta_path_to_sb_inop(mp, path);
	if (!sb_inop)
		return -EINVAL;

	*sb_inop = NULLFSINO;
	trace_xfs_imeta_sb_zap(mp, sb_inop);
	xfs_log_sb(*tpp);
	return 0;
}

/* General functions for managing metadata inode pointers */

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

/* Look up a metadata inode by its path. */
int
xfs_imeta_lookup(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path,
	xfs_ino_t			*inop)
{
	xfs_ino_t			ino;
	int				error;

	ASSERT(xfs_imeta_path_check(path));

	error = xfs_imeta_sb_lookup(mp, path, &ino);
	if (error)
		return error;

	if (!xfs_imeta_verify(mp, ino))
		return -EFSCORRUPTED;

	*inop = ino;
	return 0;
}

/*
 * Create a metadata inode with the given @mode, and insert it into the
 * metadata directory tree at the given @path.  The path (up to the final
 * component) must already exist.  The new metadata inode @ipp will be ijoined
 * and logged to @tpp, with the ILOCK held until the next transaction commit.
 * The caller must provide a @cleanup structure.
 *
 * NOTE: This function may pass a child inode @ipp back to the caller along
 * with an error status code.  The caller must always check for a non-null
 * child inode and release it.
 */
int
xfs_imeta_create(
	struct xfs_trans		**tpp,
	const struct xfs_imeta_path	*path,
	umode_t				mode,
	struct xfs_inode		**ipp,
	struct xfs_imeta_end		*cleanup)
{
	ASSERT(xfs_imeta_path_check(path));
	*ipp = NULL;

	return xfs_imeta_sb_create(tpp, path, mode, ipp);
}

/*
 * Unlink a metadata inode @ip from the metadata directory given by @path.  The
 * metadata inode must not be ILOCKed.  Upon return, the inode will be ijoined
 * and logged to @tpp, and returned with reduced link count, ready to be
 * released.  The caller must provide a @cleanup structure.
 */
int
xfs_imeta_unlink(
	struct xfs_trans		**tpp,
	const struct xfs_imeta_path	*path,
	struct xfs_inode		*ip,
	struct xfs_imeta_end		*cleanup)
{
	ASSERT(xfs_imeta_path_check(path));
	ASSERT(xfs_imeta_verify((*tpp)->t_mountp, ip->i_ino));

	return xfs_imeta_sb_unlink(tpp, path, ip);
}

/*
 * Forcibly clear the metadata pointer noted by @path so that a subsequent
 * lookup will return NULLFSINO.  If the pointer was not already NULLFSINO, the
 * caller is responsible for cleaning up those resources; in other words, this
 * function is only to be used when blowing out a totally destroyed metadata
 * inode.  The caller must provide a @cleanup structure.
 */
int
xfs_imeta_zap(
	struct xfs_trans		**tpp,
	const struct xfs_imeta_path	*path,
	struct xfs_imeta_end		*cleanup)
{
	ASSERT(xfs_imeta_path_check(path));

	return xfs_imeta_sb_zap(tpp, path);
}

/*
 * Clean up after committing (or cancelling) a metadata inode creation or
 * removal.
 */
void
xfs_imeta_end_update(
	struct xfs_mount		*mp,
	struct xfs_imeta_end		*cleanup,
	int				error)
{
	trace_xfs_imeta_end_update(mp, 0, error, _RET_IP_);
}

/* Does this inode number refer to a static metadata inode? */
bool
xfs_is_static_meta_ino(
	struct xfs_mount		*mp,
	xfs_ino_t			ino)
{
	const struct xfs_imeta_sbmap	*p;

	if (ino == NULLFSINO)
		return false;

	for (p = xfs_imeta_sbmaps; p->path; p++)
		if (ino == *xfs_imeta_sbmap_to_inop(mp, p))
			return true;

	return false;
}

/* Ensure that the in-core superblock has all the values that it should. */
int
xfs_imeta_mount(
	struct xfs_mount	*mp)
{
	return 0;
}

/* Calculate the log block reservation to create a metadata inode. */
unsigned int
xfs_imeta_create_space_res(
	struct xfs_mount	*mp)
{
	return XFS_IALLOC_SPACE_RES(mp);
}

/* Calculate the log block reservation to unlink a metadata inode. */
unsigned int
xfs_imeta_unlink_space_res(
	struct xfs_mount	*mp)
{
	return XFS_REMOVE_SPACE_RES(mp);
}
