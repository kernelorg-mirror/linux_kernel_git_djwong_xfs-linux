// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
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
#include "xfs_health.h"
#include "scrub/scrub.h"
#include "scrub/health.h"
#include "scrub/common.h"

static const unsigned int xchk_type_to_health_flag[XFS_SCRUB_TYPE_NR] = {
	[XFS_SCRUB_TYPE_SB]		= XFS_HEALTH_AG_SB,
	[XFS_SCRUB_TYPE_AGF]		= XFS_HEALTH_AG_AGF,
	[XFS_SCRUB_TYPE_AGFL]		= XFS_HEALTH_AG_AGFL,
	[XFS_SCRUB_TYPE_AGI]		= XFS_HEALTH_AG_AGI,
	[XFS_SCRUB_TYPE_BNOBT]		= XFS_HEALTH_AG_BNOBT,
	[XFS_SCRUB_TYPE_CNTBT]		= XFS_HEALTH_AG_CNTBT,
	[XFS_SCRUB_TYPE_INOBT]		= XFS_HEALTH_AG_INOBT,
	[XFS_SCRUB_TYPE_FINOBT]		= XFS_HEALTH_AG_FINOBT,
	[XFS_SCRUB_TYPE_RMAPBT]		= XFS_HEALTH_AG_RMAPBT,
	[XFS_SCRUB_TYPE_REFCNTBT]	= XFS_HEALTH_AG_REFCNTBT,
	[XFS_SCRUB_TYPE_INODE]		= XFS_HEALTH_INO_CORE,
	[XFS_SCRUB_TYPE_BMBTD]		= XFS_HEALTH_INO_BMBTD,
	[XFS_SCRUB_TYPE_BMBTA]		= XFS_HEALTH_INO_BMBTA,
	[XFS_SCRUB_TYPE_BMBTC]		= XFS_HEALTH_INO_BMBTC,
	[XFS_SCRUB_TYPE_DIR]		= XFS_HEALTH_INO_DIR,
	[XFS_SCRUB_TYPE_XATTR]		= XFS_HEALTH_INO_XATTR,
	[XFS_SCRUB_TYPE_SYMLINK]	= XFS_HEALTH_INO_SYMLINK,
	[XFS_SCRUB_TYPE_PARENT]		= XFS_HEALTH_INO_PARENT,
	[XFS_SCRUB_TYPE_RTBITMAP]	= XFS_HEALTH_RT_BITMAP,
	[XFS_SCRUB_TYPE_RTSUM]		= XFS_HEALTH_RT_SUMMARY,
	[XFS_SCRUB_TYPE_UQUOTA]		= XFS_HEALTH_FS_UQUOTA,
	[XFS_SCRUB_TYPE_GQUOTA]		= XFS_HEALTH_FS_GQUOTA,
	[XFS_SCRUB_TYPE_PQUOTA]		= XFS_HEALTH_FS_PQUOTA,
	[XFS_SCRUB_TYPE_FSCOUNTERS]	= XFS_HEALTH_FS_COUNTERS,
	[XFS_SCRUB_TYPE_RTRMAPBT]	= XFS_HEALTH_RT_RMAPBT,
};

/* Return the health status mask for this scrub type. */
unsigned int
xchk_health_mask_for_scrub_type(
	__u32			scrub_type)
{
	return xchk_type_to_health_flag[scrub_type];
}

/*
 * Quick scan to double-check that there isn't any evidence of lingering
 * primary health problems.  If we're still clear, then the health update will
 * take care of clearing the indirect evidence.
 */
int
xchk_health_record(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno;
	unsigned int		sick;

	sick = xfs_fs_measure_sickness(mp);
	if (sick & XFS_HEALTH_FS_PRIMARY)
		xchk_set_corrupt(sc);

	sick = xfs_rt_measure_sickness(mp);
	if (sick & XFS_HEALTH_RT_PRIMARY)
		xchk_set_corrupt(sc);

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		pag = xfs_perag_get(mp, agno);
		sick = xfs_ag_measure_sickness(pag);
		if (sick & XFS_HEALTH_AG_PRIMARY)
			xchk_set_corrupt(sc);
		xfs_perag_put(pag);
	}

	return 0;
}

/*
 * Scrub gave the filesystem a clean bill of health, so clear all the indirect
 * markers of past problems (at least for the fs and ags) so that we can be
 * healthy again.
 */
STATIC void
xchk_mark_all_healthy(
	struct xfs_mount	*mp)
{
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno;
	int			error = 0;

	xfs_fs_mark_healthy(mp, XFS_HEALTH_FS_INDIRECT);
	xfs_rt_mark_healthy(mp, XFS_HEALTH_RT_INDIRECT);
	for (agno = 0; error == 0 && agno < mp->m_sb.sb_agcount; agno++) {
		pag = xfs_perag_get(mp, agno);
		xfs_ag_mark_healthy(pag, XFS_HEALTH_AG_INDIRECT);
		xfs_perag_put(pag);
	}
}
/* Mark metadata unhealthy. */
static void
xchk_mark_sick(
	struct xfs_scrub	*sc,
	unsigned int		mask)
{
	struct xfs_perag	*pag;

	if (!mask)
		return;

	switch (sc->sm->sm_type) {
	case XFS_SCRUB_TYPE_SB:
	case XFS_SCRUB_TYPE_AGF:
	case XFS_SCRUB_TYPE_AGFL:
	case XFS_SCRUB_TYPE_AGI:
	case XFS_SCRUB_TYPE_BNOBT:
	case XFS_SCRUB_TYPE_CNTBT:
	case XFS_SCRUB_TYPE_INOBT:
	case XFS_SCRUB_TYPE_FINOBT:
	case XFS_SCRUB_TYPE_RMAPBT:
	case XFS_SCRUB_TYPE_REFCNTBT:
		pag = xfs_perag_get(sc->mp, sc->sm->sm_agno);
		xfs_ag_mark_sick(pag, mask);
		xfs_perag_put(pag);
		break;
	case XFS_SCRUB_TYPE_INODE:
	case XFS_SCRUB_TYPE_BMBTD:
	case XFS_SCRUB_TYPE_BMBTA:
	case XFS_SCRUB_TYPE_BMBTC:
	case XFS_SCRUB_TYPE_DIR:
	case XFS_SCRUB_TYPE_XATTR:
	case XFS_SCRUB_TYPE_SYMLINK:
	case XFS_SCRUB_TYPE_PARENT:
		/*
		 * If we're coming in for repairs then we don't want sickness
		 * flags to propagate to the fs health measure if the inode
		 * gets inactivated before we can fix it.
		 */
		if (sc->sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR)
			mask |= XFS_HEALTH_INO_FORGET;
		xfs_inode_mark_sick(sc->ip, mask);
		break;
	case XFS_SCRUB_TYPE_UQUOTA:
	case XFS_SCRUB_TYPE_GQUOTA:
	case XFS_SCRUB_TYPE_PQUOTA:
		xfs_fs_mark_sick(sc->mp, mask);
		break;
	case XFS_SCRUB_TYPE_RTBITMAP:
	case XFS_SCRUB_TYPE_RTSUM:
	case XFS_SCRUB_TYPE_RTRMAPBT:
		xfs_rt_mark_sick(sc->mp, mask);
		break;
	default:
		break;
	}
}

/* Mark metadata healed after a repair or healthy after a clean scan. */
static void
xchk_mark_healthy(
	struct xfs_scrub	*sc,
	unsigned int		mask)
{
	struct xfs_perag	*pag;

	if (!mask)
		return;

	switch (sc->sm->sm_type) {
	case XFS_SCRUB_TYPE_SB:
	case XFS_SCRUB_TYPE_AGF:
	case XFS_SCRUB_TYPE_AGFL:
	case XFS_SCRUB_TYPE_AGI:
	case XFS_SCRUB_TYPE_BNOBT:
	case XFS_SCRUB_TYPE_CNTBT:
	case XFS_SCRUB_TYPE_INOBT:
	case XFS_SCRUB_TYPE_FINOBT:
	case XFS_SCRUB_TYPE_RMAPBT:
	case XFS_SCRUB_TYPE_REFCNTBT:
		pag = xfs_perag_get(sc->mp, sc->sm->sm_agno);
		xfs_ag_mark_healthy(pag, mask);
		xfs_perag_put(pag);
		break;
	case XFS_SCRUB_TYPE_INODE:
	case XFS_SCRUB_TYPE_BMBTD:
	case XFS_SCRUB_TYPE_BMBTA:
	case XFS_SCRUB_TYPE_BMBTC:
	case XFS_SCRUB_TYPE_DIR:
	case XFS_SCRUB_TYPE_XATTR:
	case XFS_SCRUB_TYPE_SYMLINK:
	case XFS_SCRUB_TYPE_PARENT:
		xfs_inode_mark_healthy(sc->ip, mask);
		break;
	case XFS_SCRUB_TYPE_UQUOTA:
	case XFS_SCRUB_TYPE_GQUOTA:
	case XFS_SCRUB_TYPE_PQUOTA:
		xfs_fs_mark_healthy(sc->mp, mask);
		break;
	case XFS_SCRUB_TYPE_RTBITMAP:
	case XFS_SCRUB_TYPE_RTSUM:
	case XFS_SCRUB_TYPE_RTRMAPBT:
		xfs_rt_mark_healthy(sc->mp, mask);
		break;
	case XFS_SCRUB_TYPE_HEALTHY:
		xchk_mark_all_healthy(sc->mp);
		break;
	default:
		break;
	}
}

/* Update filesystem health assessments based on what we found and did. */
void
xchk_update_health(
	struct xfs_scrub	*sc,
	bool			already_fixed)
{
	/*
	 * If the scrubber finds errors, we mark sick whatever's mentioned in
	 * sick_mask, no matter whether this is a first scan or an evaluation
	 * of repair effectiveness.
	 *
	 * If there is no direct corruption and we're called after a repair,
	 * clear whatever's in heal_mask because that's what we fixed.
	 *
	 * Otherwise, there's no direct corruption and we didn't repair
	 * anything, so mark whatever's in sick_mask as healthy.
	 */
	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		xchk_mark_sick(sc, sc->sick_mask);
	else if (already_fixed)
		xchk_mark_healthy(sc, sc->heal_mask);
	else
		xchk_mark_healthy(sc, sc->sick_mask);
}
