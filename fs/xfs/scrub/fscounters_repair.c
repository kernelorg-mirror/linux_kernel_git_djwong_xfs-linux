// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_btree.h"
#include "xfs_bit.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_inode.h"
#include "xfs_alloc.h"
#include "xfs_ialloc.h"
#include "xfs_rmap.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"

/*
 * FS Summary Counters
 * ===================
 *
 * To repair the filesystem summary counters we compute the correct values,
 * take the difference between those values and the ones in m_sb, and modify
 * both the percpu and the m_sb counters by the corresponding amounts.  The
 * filesystem must be frozen to do anything.
 */

/*
 * Reset the superblock counters.
 *
 * The filesystem must be frozen so that the counters do not change while
 * we're computing the summary counters.
 */
int
xrep_fscounters(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xchk_fscounters	*fsc = sc->buf;
	int64_t			delta_icount;
	int64_t			delta_ifree;
	int64_t			delta_fdblocks;
	int			error;

	/*
	 * Reinitialize the counters.  We know that the counters in mp->m_sb
	 * are supposed to match the counters we calculated, so we therefore
	 * need to calculate the deltas...
	 */
	spin_lock(&mp->m_sb_lock);
	delta_icount = (int64_t)fsc->icount - mp->m_sb.sb_icount;
	delta_ifree = (int64_t)fsc->ifree - mp->m_sb.sb_ifree;
	delta_fdblocks = (int64_t)fsc->fdblocks - mp->m_sb.sb_fdblocks;
	spin_unlock(&mp->m_sb_lock);

	trace_xrep_reset_counters(mp, delta_icount, delta_ifree,
			delta_fdblocks);

	/* ...and then update the per-cpu counters... */
	if (delta_icount) {
		error = xfs_mod_icount(mp, delta_icount);
		if (error)
			return error;
	}
	if (delta_ifree) {
		error = xfs_mod_ifree(mp, delta_ifree);
		if (error)
			goto err_icount;
	}
	if (delta_fdblocks) {
		error = xfs_mod_fdblocks(mp, delta_fdblocks, false);
		if (error)
			goto err_ifree;
	}

	/* ...and finally log the superblock changes. */
	spin_lock(&mp->m_sb_lock);
	mp->m_sb.sb_icount = fsc->icount;
	mp->m_sb.sb_ifree = fsc->ifree;
	mp->m_sb.sb_fdblocks = fsc->fdblocks;
	mp->m_flags &= ~XFS_MOUNT_BAD_SUMMARY;
	spin_unlock(&mp->m_sb_lock);
	xfs_log_sb(sc->tp);

	return 0;
err_icount:
	xfs_mod_icount(mp, -delta_icount);
err_ifree:
	xfs_mod_ifree(mp, -delta_ifree);
	return error;
}
