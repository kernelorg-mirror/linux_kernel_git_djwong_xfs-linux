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
	xfs_rgnumber_t		rgno = sc->sm->sm_agno;
	xfs_rtblock_t		rtbno;
	int			error;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;

	error = xchk_rt_init(sc, &sc->sr);
	if (!xchk_xref_process_rt_error(sc, rgno, 0, &error))
		return;

	rtbno = xfs_rgbno_to_rtb(mp, rgno, 0);
	xchk_xref_is_used_rt_space(sc, rtbno, 1);
}

int
xchk_rgsuperblock(
	struct xfs_scrub	*sc)
{
	struct xfs_buf		*bp;
	struct xfs_rtgroup	*rtg;
	xfs_rgnumber_t		rgno;
	int			error;

	rgno = sc->sm->sm_agno;
	if (rgno == 0)
		return 0;

	/*
	 * Grab an active reference to the rtgroup structure.  If we can't get
	 * it, we're racing with something that's tearing down the group, so
	 * signal that the group no longer exists.
	 */
	rtg = xfs_rtgroup_get(sc->mp, rgno);
	if (!rtg)
		return -ENOENT;

	/* Everything in the rt super is checked by the read verifier. */
	error = xfs_buf_read_uncached(sc->mp->m_rtdev_targp, XFS_RTSB_DADDR,
			XFS_FSB_TO_BB(sc->mp, 1), 0, &bp, &xfs_rtsb_buf_ops);
	if (!xchk_process_rt_error(sc, rgno, 0, &error))
		goto out_rtg;

	xchk_rgsuperblock_xref(sc);
	xfs_buf_relse(bp);
out_rtg:
	xfs_rtgroup_put(rtg);
	return error;
}
