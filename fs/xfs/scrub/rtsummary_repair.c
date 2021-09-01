// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
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
#include "xfs_rtalloc.h"
#include "xfs_inode.h"
#include "xfs_bit.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_swapext.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/tempfile.h"

/* Set us up to repair the rtsummary file. */
int
xrep_setup_rtsummary(
	struct xfs_scrub	*sc,
	unsigned int		*resblks)
{
	struct xfs_mount	*mp = sc->mp;
	unsigned long long	blocks;
	int			error;

	error = xrep_tempfile_create(sc, S_IFREG);
	if (error)
		return error;

	/*
	 * If we're doing a repair, we reserve enough blocks to write out a
	 * completely new summary file, plus twice as many blocks as we would
	 * need if we can only allocate one block per data fork mapping.  This
	 * should cover the preallocation of the temporary file and swapping
	 * the extent mappings.
	 *
	 * We cannot use xfs_swapext_estimate because we have not yet
	 * constructed the replacement rtsummary and therefore do not know how
	 * many extents it will use.  By the time we do, we will have a dirty
	 * transaction (which we cannot drop because we cannot drop the
	 * rtsummary ILOCK) and cannot ask for more reservation.
	 */
	blocks = XFS_B_TO_FSB(mp, mp->m_rsumsize);
	blocks += xfs_bmbt_calc_size(mp, blocks) * 2;
	if (blocks > UINT_MAX)
		return -EOPNOTSUPP;

	*resblks = blocks;
	return 0;
}

/* Repair the realtime summary. */
int
xrep_rtsummary(
	struct xfs_scrub	*sc)
{
	struct xfs_swapext_req	req = { .req_flags = 0 };
	int			error;

	/* Make sure any problems with the fork are fixed. */
	error = xrep_metadata_inode_forks(sc);
	if (error)
		return error;

	/*
	 * Trylock the temporary file.  We had better be the only ones holding
	 * onto this inode...
	 */
	if (!xrep_tempfile_ilock_nowait(sc, XFS_ILOCK_EXCL))
		return -EAGAIN;

	/* Make sure we have space allocated for the entire summary file. */
	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	xfs_trans_ijoin(sc->tp, sc->tempip, 0);
	error = xrep_tempfile_prealloc(sc, 0,
			XFS_B_TO_FSB(sc->mp, sc->mp->m_rsumsize));
	if (error)
		return error;

	/* Copy the rtsummary file that we generated. */
	error = xrep_tempfile_copyin(sc, &xfs_rtbuf_ops,
			XFS_BLFT_RTSUMMARY_BUF, sc->mp->m_rsumsize);
	if (error)
		return error;

	/* Now swap the extents. */
	req.ip1 = sc->tempip;
	req.ip2 = sc->ip;
	req.whichfork = XFS_DATA_FORK;
	req.blockcount = XFS_B_TO_FSB(sc->mp, sc->mp->m_rsumsize);
	return xfs_swapext(&sc->tp, &req);
}
