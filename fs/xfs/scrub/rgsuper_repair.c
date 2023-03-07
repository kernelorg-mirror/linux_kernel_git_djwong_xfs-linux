// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_btree.h"
#include "xfs_inode.h"
#include "xfs_bit.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_rtgroup.h"
#include "xfs_sb.h"
#include "scrub/scrub.h"
#include "scrub/repair.h"

int
xrep_rgsuperblock(
	struct xfs_scrub	*sc)
{
	struct xfs_buf		*bp;
	int			error;

	/*
	 * If this is the primary rtgroup superblock, log a superblock update
	 * to force both to disk.
	 */
	if (sc->sr.rtg->rtg_rgno == 0) {
		xfs_log_sb(sc->tp);
		return 0;
	}

	/* Otherwise just write a new secondary to disk directly. */
	error = xfs_rtgroup_init_secondary_super(sc->mp, sc->sr.rtg->rtg_rgno,
			&bp);
	if (error)
		return error;

	error = xfs_bwrite(bp);
	xfs_buf_relse(bp);
	return error;
}
