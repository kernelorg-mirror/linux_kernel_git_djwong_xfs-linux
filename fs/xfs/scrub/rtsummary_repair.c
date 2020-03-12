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
	return xfs_rtbuf_get(sc->mp, sc->tp, off, 1, bpp);
}

/* Repair the realtime summary. */
int
xrep_rtsummary(
	struct xfs_scrub	*sc)
{
	int			error;

	/* Make sure any problems with the fork are fixed. */
	error = xrep_metadata_inode_forks(sc);
	if (error)
		return error;

	/* Make sure we have space allocated for the entire summary file. */
	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	error = xrep_fallocate(sc, 0, XFS_B_TO_FSB(sc->mp, sc->mp->m_rsumsize));
	if (error)
		return error;

	/* Copy the rtsummary file that we generated. */
	return xrep_set_file_contents(sc, xrep_rtsum_get_buf, sc->xfile,
			sc->mp->m_rsumsize);
}
