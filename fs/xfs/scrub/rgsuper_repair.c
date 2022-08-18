// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
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
#include "scrub/scrub.h"
#include "scrub/repair.h"

int
xrep_rgsuperblock(
	struct xfs_scrub	*sc)
{
	struct xfs_buf		*bp;
	struct xfs_rtgroup	*rtg;
	xfs_rgnumber_t		rgno;
	int			error;

	rgno = sc->sm->sm_agno;
	if (rgno == 0)
		return 0;

	rtg = xfs_rtgroup_get(sc->mp, rgno);
	if (!rtg)
		return -ENOENT;

	error = xfs_rtgroup_init_secondary_super(sc->mp, rgno, &bp);
	if (error)
		goto out_rtg;

	error = xfs_bwrite(bp);
	if (error)
		goto out_buf;
out_buf:
	xfs_buf_relse(bp);
out_rtg:
	xfs_rtgroup_put(rtg);
	return error;
}
