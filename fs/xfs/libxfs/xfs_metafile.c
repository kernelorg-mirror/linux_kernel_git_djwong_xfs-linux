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
#include "xfs_metafile.h"
#include "xfs_trace.h"
#include "xfs_inode.h"

/* Set up an inode to be recognized as a metadata directory inode. */
void
xfs_metafile_set_iflag(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	VFS_I(ip)->i_mode &= ~0777;
	VFS_I(ip)->i_uid = GLOBAL_ROOT_UID;
	VFS_I(ip)->i_gid = GLOBAL_ROOT_GID;
	ip->i_diflags |= (XFS_DIFLAG_IMMUTABLE | XFS_DIFLAG_SYNC |
			  XFS_DIFLAG_NOATIME | XFS_DIFLAG_NODUMP |
			  XFS_DIFLAG_NODEFRAG);
	if (S_ISDIR(VFS_I(ip)->i_mode))
		ip->i_diflags |= XFS_DIFLAG_NOSYMLINKS;
	ip->i_diflags2 &= ~XFS_DIFLAG2_DAX;
	ip->i_diflags2 |= XFS_DIFLAG2_METADATA;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
}

/* Clear the metadata directory inode flag. */
void
xfs_metafile_clear_iflag(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	ASSERT(xfs_is_metadir_inode(ip));
	ASSERT(VFS_I(ip)->i_nlink == 0);

	ip->i_diflags2 &= ~XFS_DIFLAG2_METADATA;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
}
