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
#include "xfs_btree.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_inode.h"
#include "xfs_bit.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/xfile.h"

/* Set up to repair the realtime bitmap. */
int
xrep_setup_rtbitmap(
	struct xfs_scrub	*sc,
	unsigned int		*resblks)
{
	struct xfs_mount	*mp = sc->mp;
	unsigned long long	blocks = 0;

	/*
	 * Reserve enough blocks to write out a completely new bmbt for the
	 * bitmap file.
	 */
	blocks = xfs_bmbt_calc_size(mp, mp->m_sb.sb_rbmblocks);
	if (blocks > UINT_MAX)
		return -EOPNOTSUPP;

	*resblks += blocks;
	return 0;
}

/* Repair the realtime bitmap. */
int
xrep_rtbitmap(
	struct xfs_scrub	*sc)
{
	/*
	 * The only thing we know how to fix right now is problems with the
	 * inode or its fork data.
	 */
	return xrep_metadata_inode_forks(sc);
}
