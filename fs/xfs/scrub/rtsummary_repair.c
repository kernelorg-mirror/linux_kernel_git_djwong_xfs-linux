// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
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
#include "xfs_rtalloc.h"
#include "xfs_inode.h"
#include "xfs_bit.h"
#include "xfs_bmap.h"
#include "xfs_swapext.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"

/* Retrieve one block from the rtsummary file. */
STATIC int
xrep_rtsum_get_buf(
	struct xfs_scrub	*sc,
	xfs_fileoff_t		off,
	struct xfs_buf		**bpp)
{
	struct xfs_bmbt_irec	map;
	struct xfs_buf		*bp;
	struct xfs_mount	*mp = sc->mp;
	int			nmap = 1;
	int			error;

	error = xfs_bmapi_read(sc->tempip, off, 1, &map, &nmap,
			XFS_DATA_FORK);
	if (error)
		return error;

	if (nmap == 0 || !xfs_bmap_is_real_extent(&map))
		return -EFSCORRUPTED;

	error = xfs_trans_read_buf(mp, sc->tp, mp->m_ddev_targp,
			XFS_FSB_TO_DADDR(mp, map.br_startblock),
			mp->m_bsize, 0, &bp, &xfs_rtbuf_ops);
	if (error)
		return error;

	xfs_trans_buf_set_type(sc->tp, bp, XFS_BLFT_RTSUMMARY_BUF);
	*bpp = bp;
	return 0;
}

/* Repair the realtime summary. */
int
xrep_rtsummary(
	struct xfs_scrub	*sc)
{
	int			error;

	/* We require atomic file swap to be able to fix rt summaries. */
	if (!xfs_sb_version_hasatomicswap(&sc->mp->m_sb))
		return -EOPNOTSUPP;

	/* Make sure any problems with the fork are fixed. */
	error = xrep_metadata_inode_forks(sc);
	if (error)
		return error;

	/*
	 * Trylock the temporary file.  We had better be the only ones holding
	 * onto this inode...
	 */
	if (!xfs_ilock_nowait(sc->tempip, XFS_ILOCK_EXCL))
		return -EAGAIN;
	sc->temp_ilock_flags = XFS_ILOCK_EXCL;

	/* Make sure we have space allocated for the entire summary file. */
	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	xfs_trans_ijoin(sc->tp, sc->tempip, 0);
	error = xrep_fallocate(sc, 0, XFS_B_TO_FSB(sc->mp, sc->mp->m_rsumsize));
	if (error)
		return error;

	/* Copy the rtsummary file that we generated. */
	error = xrep_set_file_contents(sc, xrep_rtsum_get_buf, sc->xfile,
			sc->mp->m_rsumsize);
	if (error)
		return error;

	/* Now swap the extents. */
	return xfs_swapext_atomic(&sc->tp, sc->ip, sc->tempip, XFS_DATA_FORK,
			0, 0, XFS_B_TO_FSB(sc->mp, sc->mp->m_rsumsize), 0);
}
