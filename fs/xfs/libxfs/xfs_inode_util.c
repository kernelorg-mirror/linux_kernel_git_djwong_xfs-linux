// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include <linux/iversion.h>
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_inode_util.h"
#include "xfs_trans.h"
#include "xfs_ialloc.h"
#include "xfs_health.h"

uint16_t
xfs_flags2diflags(
	struct xfs_inode	*ip,
	unsigned int		xflags)
{
	/* can't set PREALLOC this way, just preserve it */
	uint16_t		di_flags =
		(ip->i_d.di_flags & XFS_DIFLAG_PREALLOC);

	if (xflags & FS_XFLAG_IMMUTABLE)
		di_flags |= XFS_DIFLAG_IMMUTABLE;
	if (xflags & FS_XFLAG_APPEND)
		di_flags |= XFS_DIFLAG_APPEND;
	if (xflags & FS_XFLAG_SYNC)
		di_flags |= XFS_DIFLAG_SYNC;
	if (xflags & FS_XFLAG_NOATIME)
		di_flags |= XFS_DIFLAG_NOATIME;
	if (xflags & FS_XFLAG_NODUMP)
		di_flags |= XFS_DIFLAG_NODUMP;
	if (xflags & FS_XFLAG_NODEFRAG)
		di_flags |= XFS_DIFLAG_NODEFRAG;
	if (xflags & FS_XFLAG_FILESTREAM)
		di_flags |= XFS_DIFLAG_FILESTREAM;
	if (S_ISDIR(VFS_I(ip)->i_mode)) {
		if (xflags & FS_XFLAG_RTINHERIT)
			di_flags |= XFS_DIFLAG_RTINHERIT;
		if (xflags & FS_XFLAG_NOSYMLINKS)
			di_flags |= XFS_DIFLAG_NOSYMLINKS;
		if (xflags & FS_XFLAG_EXTSZINHERIT)
			di_flags |= XFS_DIFLAG_EXTSZINHERIT;
		if (xflags & FS_XFLAG_PROJINHERIT)
			di_flags |= XFS_DIFLAG_PROJINHERIT;
	} else if (S_ISREG(VFS_I(ip)->i_mode)) {
		if (xflags & FS_XFLAG_REALTIME)
			di_flags |= XFS_DIFLAG_REALTIME;
		if (xflags & FS_XFLAG_EXTSIZE)
			di_flags |= XFS_DIFLAG_EXTSIZE;
	}

	return di_flags;
}

uint64_t
xfs_flags2diflags2(
	struct xfs_inode	*ip,
	unsigned int		xflags)
{
	uint64_t		di_flags2 =
		(ip->i_d.di_flags2 & (XFS_DIFLAG2_REFLINK |
				      XFS_DIFLAG2_BIGTIME));

	if (xflags & FS_XFLAG_DAX)
		di_flags2 |= XFS_DIFLAG2_DAX;
	if (xflags & FS_XFLAG_COWEXTSIZE)
		di_flags2 |= XFS_DIFLAG2_COWEXTSIZE;

	return di_flags2;
}

uint32_t
xfs_dic2xflags(
	uint16_t		di_flags,
	uint64_t		di_flags2,
	bool			has_attr)
{
	uint32_t		flags = 0;

	if (di_flags & XFS_DIFLAG_ANY) {
		if (di_flags & XFS_DIFLAG_REALTIME)
			flags |= FS_XFLAG_REALTIME;
		if (di_flags & XFS_DIFLAG_PREALLOC)
			flags |= FS_XFLAG_PREALLOC;
		if (di_flags & XFS_DIFLAG_IMMUTABLE)
			flags |= FS_XFLAG_IMMUTABLE;
		if (di_flags & XFS_DIFLAG_APPEND)
			flags |= FS_XFLAG_APPEND;
		if (di_flags & XFS_DIFLAG_SYNC)
			flags |= FS_XFLAG_SYNC;
		if (di_flags & XFS_DIFLAG_NOATIME)
			flags |= FS_XFLAG_NOATIME;
		if (di_flags & XFS_DIFLAG_NODUMP)
			flags |= FS_XFLAG_NODUMP;
		if (di_flags & XFS_DIFLAG_RTINHERIT)
			flags |= FS_XFLAG_RTINHERIT;
		if (di_flags & XFS_DIFLAG_PROJINHERIT)
			flags |= FS_XFLAG_PROJINHERIT;
		if (di_flags & XFS_DIFLAG_NOSYMLINKS)
			flags |= FS_XFLAG_NOSYMLINKS;
		if (di_flags & XFS_DIFLAG_EXTSIZE)
			flags |= FS_XFLAG_EXTSIZE;
		if (di_flags & XFS_DIFLAG_EXTSZINHERIT)
			flags |= FS_XFLAG_EXTSZINHERIT;
		if (di_flags & XFS_DIFLAG_NODEFRAG)
			flags |= FS_XFLAG_NODEFRAG;
		if (di_flags & XFS_DIFLAG_FILESTREAM)
			flags |= FS_XFLAG_FILESTREAM;
	}

	if (di_flags2 & XFS_DIFLAG2_ANY) {
		if (di_flags2 & XFS_DIFLAG2_DAX)
			flags |= FS_XFLAG_DAX;
		if (di_flags2 & XFS_DIFLAG2_COWEXTSIZE)
			flags |= FS_XFLAG_COWEXTSIZE;
	}

	if (has_attr)
		flags |= FS_XFLAG_HASATTR;

	return flags;
}

#define XFS_PROJID_DEFAULT	0

prid_t
xfs_get_initial_prid(
	struct xfs_inode	*dp)
{
	if (dp->i_d.di_flags & XFS_DIFLAG_PROJINHERIT)
		return dp->i_d.di_projid;

	return XFS_PROJID_DEFAULT;
}

/* Propagate di_flags from a parent inode to a child inode. */
static void
xfs_inode_inherit_flags(
	struct xfs_inode	*ip,
	const struct xfs_inode	*pip)
{
	unsigned int		di_flags = 0;
	umode_t			mode = VFS_I(ip)->i_mode;

	if (S_ISDIR(mode)) {
		if (pip->i_d.di_flags & XFS_DIFLAG_RTINHERIT)
			di_flags |= XFS_DIFLAG_RTINHERIT;
		if (pip->i_d.di_flags & XFS_DIFLAG_EXTSZINHERIT) {
			di_flags |= XFS_DIFLAG_EXTSZINHERIT;
			ip->i_d.di_extsize = pip->i_d.di_extsize;
		}
		if (pip->i_d.di_flags & XFS_DIFLAG_PROJINHERIT)
			di_flags |= XFS_DIFLAG_PROJINHERIT;
	} else if (S_ISREG(mode)) {
		if ((pip->i_d.di_flags & XFS_DIFLAG_RTINHERIT) &&
		    xfs_sb_version_hasrealtime(&ip->i_mount->m_sb))
			di_flags |= XFS_DIFLAG_REALTIME;
		if (pip->i_d.di_flags & XFS_DIFLAG_EXTSZINHERIT) {
			di_flags |= XFS_DIFLAG_EXTSIZE;
			ip->i_d.di_extsize = pip->i_d.di_extsize;
		}
	}
	if ((pip->i_d.di_flags & XFS_DIFLAG_NOATIME) &&
	    xfs_inherit_noatime)
		di_flags |= XFS_DIFLAG_NOATIME;
	if ((pip->i_d.di_flags & XFS_DIFLAG_NODUMP) &&
	    xfs_inherit_nodump)
		di_flags |= XFS_DIFLAG_NODUMP;
	if ((pip->i_d.di_flags & XFS_DIFLAG_SYNC) &&
	    xfs_inherit_sync)
		di_flags |= XFS_DIFLAG_SYNC;
	if ((pip->i_d.di_flags & XFS_DIFLAG_NOSYMLINKS) &&
	    xfs_inherit_nosymlinks)
		di_flags |= XFS_DIFLAG_NOSYMLINKS;
	if ((pip->i_d.di_flags & XFS_DIFLAG_NODEFRAG) &&
	    xfs_inherit_nodefrag)
		di_flags |= XFS_DIFLAG_NODEFRAG;
	if (pip->i_d.di_flags & XFS_DIFLAG_FILESTREAM)
		di_flags |= XFS_DIFLAG_FILESTREAM;

	ip->i_d.di_flags |= di_flags;
}

/* Propagate di_flags2 from a parent inode to a child inode. */
static void
xfs_inode_inherit_flags2(
	struct xfs_inode	*ip,
	const struct xfs_inode	*pip)
{
	if (pip->i_d.di_flags2 & XFS_DIFLAG2_COWEXTSIZE) {
		ip->i_d.di_flags2 |= XFS_DIFLAG2_COWEXTSIZE;
		ip->i_d.di_cowextsize = pip->i_d.di_cowextsize;
	}
	if (pip->i_d.di_flags2 & XFS_DIFLAG2_DAX)
		ip->i_d.di_flags2 |= XFS_DIFLAG2_DAX;
}

/* Initialise an inode's attributes. */
void
xfs_inode_init(
	struct xfs_trans	*tp,
	const struct xfs_ialloc_args *args,
	struct xfs_inode	*ip)
{
	struct xfs_inode	*pip = args->pip;
	struct inode		*dir = pip ? VFS_I(pip) : NULL;
	struct xfs_mount	*mp = tp->t_mountp;
	struct inode		*inode = VFS_I(ip);
	unsigned int		flags;
	int			times = XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG |
					XFS_ICHGTIME_ACCESS;

	set_nlink(inode, args->nlink);
	inode->i_rdev = args->rdev;
	ip->i_d.di_projid = args->prid;

	if (dir && !(dir->i_mode & S_ISGID) &&
	    (mp->m_flags & XFS_MOUNT_GRPID)) {
		inode->i_uid = args->uid;
		inode->i_gid = dir->i_gid;
		inode->i_mode = args->mode;
	} else {
		inode_init_owner(args->mnt_userns, inode, dir, args->mode);
	}

	/*
	 * If the group ID of the new file does not match the effective group
	 * ID or one of the supplementary group IDs, the S_ISGID bit is cleared
	 * (and only if the irix_sgid_inherit compatibility variable is set).
	 */
	if (irix_sgid_inherit &&
	    (inode->i_mode & S_ISGID) &&
	    !in_group_p(i_gid_into_mnt(args->mnt_userns, inode)))
		inode->i_mode &= ~S_ISGID;

	/* struct copies */
	if (args->flags & XFS_IALLOC_ARGS_FORCE_UID)
		inode->i_uid = args->uid;
	else
		ASSERT(uid_eq(inode->i_uid, args->uid));
	if (args->flags & XFS_IALLOC_ARGS_FORCE_GID)
		inode->i_gid = args->gid;
	else if (!pip || !XFS_INHERIT_GID(pip))
		ASSERT(gid_eq(inode->i_gid, args->gid));
	if (args->flags & XFS_IALLOC_ARGS_FORCE_MODE)
		inode->i_mode = args->mode;

	ip->i_d.di_size = 0;
	ip->i_df.if_nextents = 0;
	ASSERT(ip->i_d.di_nblocks == 0);

	ip->i_d.di_extsize = 0;
	ip->i_d.di_dmevmask = 0;
	ip->i_d.di_dmstate = 0;
	ip->i_d.di_flags = 0;

	if (xfs_sb_version_has_v3inode(&mp->m_sb)) {
		inode_set_iversion(inode, 1);
		ip->i_d.di_flags2 = mp->m_ino_geo.new_diflags2;
		ip->i_d.di_cowextsize = 0;
		times |= XFS_ICHGTIME_CREATE;
	}

	xfs_trans_ichgtime(tp, ip, times);

	flags = XFS_ILOG_CORE;
	switch (args->mode & S_IFMT) {
	case S_IFIFO:
	case S_IFCHR:
	case S_IFBLK:
	case S_IFSOCK:
		ip->i_df.if_format = XFS_DINODE_FMT_DEV;
		ip->i_df.if_flags = 0;
		flags |= XFS_ILOG_DEV;
		break;
	case S_IFREG:
	case S_IFDIR:
		if (pip && (pip->i_d.di_flags & XFS_DIFLAG_ANY))
			xfs_inode_inherit_flags(ip, pip);
		if (pip && (pip->i_d.di_flags2 & XFS_DIFLAG2_ANY))
			xfs_inode_inherit_flags2(ip, pip);
		/* FALLTHROUGH */
	case S_IFLNK:
		ip->i_df.if_format = XFS_DINODE_FMT_EXTENTS;
		ip->i_df.if_flags = XFS_IFEXTENTS;
		ip->i_df.if_bytes = 0;
		ip->i_df.if_u1.if_root = NULL;
		break;
	default:
		ASSERT(0);
	}

	/*
	 * Log the new values stuffed into the inode.
	 */
	xfs_trans_ijoin(tp, ip, XFS_ILOCK_EXCL);
	xfs_trans_log_inode(tp, ip, flags);

	/* now that we have an i_mode we can setup the inode structure */
	xfs_setup_inode(ip);
}
