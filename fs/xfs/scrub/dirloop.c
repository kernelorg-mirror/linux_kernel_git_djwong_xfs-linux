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
#include "xfs_icache.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "xfs_attr.h"
#include "xfs_parent.h"
#include "scrub/scrub.h"
#include "scrub/common.h"

/* Set us up to look for directory loops. */
int
xchk_setup_dirloop(
	struct xfs_scrub		*sc)
{
	return xchk_setup_inode_contents(sc, 0);
}

/* Look for directory loops. */
int
xchk_dirloop(
	struct xfs_scrub		*sc)
{
	/*
	 * Nondirectories do not point downwards to other files, so they cannot
	 * cause a cycle in the directory tree.  Unlinked directories cannot be
	 * part of a cycle, so we skip them.
	 */
	if (!S_ISDIR(VFS_I(sc->ip)->i_mode) || VFS_I(sc->ip)->i_nlink == 0)
		return -ENOENT;

	return 0;
}
