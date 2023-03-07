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
#include "xfs_rtgroup.h"
#include "scrub/scrub.h"
#include "scrub/common.h"

/* Set us up with a transaction and an empty context. */
int
xchk_setup_rgsuperblock(
	struct xfs_scrub	*sc)
{
	return xchk_trans_alloc(sc, 0);
}

/* Cross-reference with the other rt metadata. */
STATIC void
xchk_rgsuperblock_xref(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	xfs_rgnumber_t		rgno = sc->sr.rtg->rtg_rgno;
	xfs_rtblock_t		rtbno;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;

	rtbno = xfs_rgbno_to_rtb(mp, rgno, 0);
	xchk_xref_is_used_rt_space(sc, rtbno, 1);
}

int
xchk_rgsuperblock(
	struct xfs_scrub	*sc)
{
	struct xfs_buf		*bp;
	xfs_rgnumber_t		rgno = sc->sm->sm_agno;
	int			error;

	/*
	 * Grab an active reference to the rtgroup structure.  If we can't get
	 * it, we're racing with something that's tearing down the group, so
	 * signal that the group no longer exists.  Take the rtbitmap in shared
	 * mode so that the group can't change while we're doing things.
	 */
	error = xchk_rtgroup_init(sc, rgno, &sc->sr, XFS_RTGLOCK_BITMAP_SHARED);
	if (error)
		return error;

	/*
	 * If this is the primary rtgroup superblock, we know it passed the
	 * verifier checks at mount time and do not need to load the buffer
	 * again.
	 */
	if (sc->sr.rtg->rtg_rgno == 0) {
		xchk_rgsuperblock_xref(sc);
		return 0;
	}

	/* The secondary rt super is checked by the read verifier. */
	error = xfs_buf_read_uncached(sc->mp->m_rtdev_targp, XFS_RTSB_DADDR,
			XFS_FSB_TO_BB(sc->mp, 1), 0, &bp, &xfs_rtsb_buf_ops);
	if (!xchk_process_rt_error(sc, rgno, 0, &error))
		return error;

	xchk_rgsuperblock_xref(sc);
	xfs_buf_relse(bp);
	return 0;
}
