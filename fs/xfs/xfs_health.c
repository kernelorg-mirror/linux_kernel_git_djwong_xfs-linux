// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_inode.h"
#include "xfs_trace.h"
#include "xfs_health.h"

/*
 * Try to this AG offline by hiding its free space from the fdblocks count.
 * This prevents other parts of the filesystem from increasing their reliance
 * on the broken AG.  If the AG is already offline, we don't have to do
 * anything.
 *
 * Caller must hold the per-ag state lock.
 */
STATIC void
xfs_health_try_offline_ag(
	struct xfs_perag	*pag)
{
	int			error;

	if (pag->pag_sick & XFS_HEALTH_AG_OFFLINE)
		return;

	error = xfs_mod_fdblocks(pag->pag_mount, -(int64_t)pag->pagf_freeblks,
			true);
	if (error)
		return;

	trace_xfs_ag_going_offline(pag->pag_mount, pag->pag_agno);
	pag->pag_sick |= XFS_HEALTH_AG_OFFLINE;
}

/*
 * Try to this AG back online by revealing its free space in the fdblocks
 * count.  There must not be any evidence of primary corruption.
 *
 * Caller must hold the per-ag state lock.
 */
STATIC void
xfs_health_try_online_ag(
	struct xfs_perag	*pag)
{
	if (!(pag->pag_sick & XFS_HEALTH_AG_OFFLINE))
		return;
	if (pag->pag_sick & XFS_HEALTH_AG_PRIMARY)
		return;

	xfs_mod_fdblocks(pag->pag_mount, pag->pagf_freeblks, true);
	trace_xfs_ag_going_online(pag->pag_mount, pag->pag_agno);
	pag->pag_sick &= ~XFS_HEALTH_AG_OFFLINE;
}

/*
 * Warn about metadata corruption that we detected but haven't fixed, and
 * make sure we're not sitting on anything that would get in the way of
 * recovery.
 */
void
xfs_health_unmount(
	struct xfs_mount	*mp)
{
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno;
	unsigned int		sick;
	bool			warn = false;

	if (XFS_FORCED_SHUTDOWN(mp))
		return;

	/* Measure AG corruption levels. */
	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		pag = xfs_perag_get(mp, agno);
		spin_lock(&pag->pag_state_lock);
		xfs_health_try_online_ag(pag);
		if (pag->pag_sick) {
			trace_xfs_ag_unfixed_corruption(mp, agno, sick);
			warn = true;
		}
		spin_unlock(&pag->pag_state_lock);
		xfs_perag_put(pag);
	}

	/* Measure realtime volume corruption levels. */
	sick = xfs_rt_measure_sickness(mp);
	if (sick) {
		trace_xfs_rt_unfixed_corruption(mp, sick);
		warn = true;
	}

	/* Measure fs corruption and keep the sample around for the warning. */
	sick = xfs_fs_measure_sickness(mp);
	if (sick) {
		trace_xfs_fs_unfixed_corruption(mp, sick);
		warn = true;
	}

	if (warn) {
		xfs_warn(mp,
"Uncorrected metadata errors detected; please run xfs_repair.");

		/*
		 * If we have unhealthy metadata, we want the admin to run
		 * xfs_repair after unmounting.  They can't do that if the log
		 * is written out without a clean unmount record (such as when
		 * the summary counters are marked unhealthy to force
		 * recalculation of the summary counters) so clear it.
		 */
		if (sick & XFS_HEALTH_FS_COUNTERS)
			xfs_fs_mark_healthy(mp, XFS_HEALTH_FS_COUNTERS);
	}
}

/* Mark unhealthy per-fs metadata. */
void
xfs_fs_mark_sick(
	struct xfs_mount	*mp,
	unsigned int		mask)
{
	ASSERT(!(mask & ~XFS_HEALTH_FS_ALL));
	trace_xfs_fs_mark_sick(mp, mask);

	spin_lock(&mp->m_sb_lock);
	mp->m_sick |= mask;
	spin_unlock(&mp->m_sb_lock);
}

/* Mark a per-fs metadata healed. */
void
xfs_fs_mark_healthy(
	struct xfs_mount	*mp,
	unsigned int		mask)
{
	ASSERT(!(mask & ~XFS_HEALTH_FS_ALL));
	trace_xfs_fs_mark_healthy(mp, mask);

	spin_lock(&mp->m_sb_lock);
	mp->m_sick &= ~mask;
	if (!(mp->m_sick & XFS_HEALTH_FS_PRIMARY))
		mp->m_sick &= ~XFS_HEALTH_FS_SECONDARY;
	spin_unlock(&mp->m_sb_lock);
}

/* Sample which per-fs metadata are unhealthy. */
unsigned int
xfs_fs_measure_sickness(
	struct xfs_mount	*mp)
{
	unsigned int		ret;

	spin_lock(&mp->m_sb_lock);
	ret = mp->m_sick;
	spin_unlock(&mp->m_sb_lock);
	return ret;
}

/* Mark unhealthy realtime metadata. */
void
xfs_rt_mark_sick(
	struct xfs_mount	*mp,
	unsigned int		mask)
{
	ASSERT(!(mask & ~XFS_HEALTH_RT_ALL));
	trace_xfs_rt_mark_sick(mp, mask);

	spin_lock(&mp->m_sb_lock);
	mp->m_rt_sick |= mask;
	spin_unlock(&mp->m_sb_lock);
}

/* Mark a realtime metadata healed. */
void
xfs_rt_mark_healthy(
	struct xfs_mount	*mp,
	unsigned int		mask)
{
	ASSERT(!(mask & ~XFS_HEALTH_RT_ALL));
	trace_xfs_rt_mark_healthy(mp, mask);

	spin_lock(&mp->m_sb_lock);
	mp->m_rt_sick &= ~mask;
	if (!(mp->m_rt_sick & XFS_HEALTH_RT_PRIMARY))
		mp->m_rt_sick &= ~XFS_HEALTH_RT_SECONDARY;
	spin_unlock(&mp->m_sb_lock);
}

/* Sample which realtime metadata are unhealthy. */
unsigned int
xfs_rt_measure_sickness(
	struct xfs_mount	*mp)
{
	unsigned int		ret;

	spin_lock(&mp->m_sb_lock);
	ret = mp->m_rt_sick;
	spin_unlock(&mp->m_sb_lock);
	return ret;
}

/* Mark unhealthy per-ag metadata. */
void
xfs_ag_mark_sick(
	struct xfs_perag	*pag,
	unsigned int		mask)
{
	ASSERT(!(mask & ~XFS_HEALTH_AG_ALL));
	trace_xfs_ag_mark_sick(pag->pag_mount, pag->pag_agno, mask);

	spin_lock(&pag->pag_state_lock);
	xfs_health_try_offline_ag(pag);
	pag->pag_sick |= mask;
	spin_unlock(&pag->pag_state_lock);
}

/* Mark per-ag metadata ok. */
void
xfs_ag_mark_healthy(
	struct xfs_perag	*pag,
	unsigned int		mask)
{
	ASSERT(!(mask & ~XFS_HEALTH_AG_ALL));
	trace_xfs_ag_mark_healthy(pag->pag_mount, pag->pag_agno, mask);

	spin_lock(&pag->pag_state_lock);
	pag->pag_sick &= ~mask;
	xfs_health_try_online_ag(pag);
	if (!(pag->pag_sick & XFS_HEALTH_AG_PRIMARY))
		pag->pag_sick &= ~XFS_HEALTH_AG_SECONDARY;
	spin_unlock(&pag->pag_state_lock);
}

/* Sample which per-ag metadata are unhealthy. */
unsigned int
xfs_ag_measure_sickness(
	struct xfs_perag	*pag)
{
	unsigned int		ret;

	spin_lock(&pag->pag_state_lock);
	ret = pag->pag_sick;
	spin_unlock(&pag->pag_state_lock);
	return ret;
}

/* Mark the unhealthy parts of an inode. */
void
xfs_inode_mark_sick(
	struct xfs_inode	*ip,
	unsigned int		mask)
{
	ASSERT(!(mask & ~XFS_HEALTH_INO_ALL));
	trace_xfs_inode_mark_sick(ip, mask);

	spin_lock(&ip->i_flags_lock);
	ip->i_sick |= mask;
	spin_unlock(&ip->i_flags_lock);
}

/* Mark parts of an inode healed. */
void
xfs_inode_mark_healthy(
	struct xfs_inode	*ip,
	unsigned int		mask)
{
	ASSERT(!(mask & ~XFS_HEALTH_INO_ALL));
	trace_xfs_inode_mark_healthy(ip, mask);

	spin_lock(&ip->i_flags_lock);
	ip->i_sick &= ~mask;
	if (!(ip->i_sick & XFS_HEALTH_INO_PRIMARY))
		ip->i_sick &= ~XFS_HEALTH_INO_SECONDARY;
	spin_unlock(&ip->i_flags_lock);
}

/* Sample which parts of an inode are unhealthy. */
unsigned int
xfs_inode_measure_sickness(
	struct xfs_inode	*ip)
{
	unsigned int		ret;

	spin_lock(&ip->i_flags_lock);
	ret = ip->i_sick;
	spin_unlock(&ip->i_flags_lock);
	return ret;
}

/* Fill out fs geometry health info. */
void
xfs_fsop_geom_health(
	struct xfs_mount	*mp,
	struct xfs_fsop_geom	*geo)
{
	unsigned int		sick;

	geo->health = 0;

	sick = xfs_fs_measure_sickness(mp);
	if (sick & XFS_HEALTH_FS_COUNTERS)
		geo->health |= XFS_FSOP_GEOM_HEALTH_FS_COUNTERS;
	if (sick & XFS_HEALTH_FS_UQUOTA)
		geo->health |= XFS_FSOP_GEOM_HEALTH_FS_UQUOTA;
	if (sick & XFS_HEALTH_FS_GQUOTA)
		geo->health |= XFS_FSOP_GEOM_HEALTH_FS_GQUOTA;
	if (sick & XFS_HEALTH_FS_PQUOTA)
		geo->health |= XFS_FSOP_GEOM_HEALTH_FS_PQUOTA;

	sick = xfs_rt_measure_sickness(mp);
	if (sick & XFS_HEALTH_RT_BITMAP)
		geo->health |= XFS_FSOP_GEOM_HEALTH_RT_BITMAP;
	if (sick & XFS_HEALTH_RT_SUMMARY)
		geo->health |= XFS_FSOP_GEOM_HEALTH_RT_SUMMARY;
	if (sick & XFS_HEALTH_RT_RMAPBT)
		geo->health |= XFS_FSOP_GEOM_HEALTH_RT_RMAPBT;
}

/* Fill out ag geometry health info. */
void
xfs_ag_geom_health(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct xfs_ag_geometry	*ageo)
{
	struct xfs_perag	*pag;
	unsigned int		sick;

	if (agno >= mp->m_sb.sb_agcount)
		return;

	ageo->ag_health = 0;

	pag = xfs_perag_get(mp, agno);
	sick = xfs_ag_measure_sickness(pag);
	if (sick & XFS_HEALTH_AG_SB)
		ageo->ag_health |= XFS_AG_GEOM_HEALTH_AG_SB;
	if (sick & XFS_HEALTH_AG_AGF)
		ageo->ag_health |= XFS_AG_GEOM_HEALTH_AG_AGF;
	if (sick & XFS_HEALTH_AG_AGFL)
		ageo->ag_health |= XFS_AG_GEOM_HEALTH_AG_AGFL;
	if (sick & XFS_HEALTH_AG_AGI)
		ageo->ag_health |= XFS_AG_GEOM_HEALTH_AG_AGI;
	if (sick & XFS_HEALTH_AG_BNOBT)
		ageo->ag_health |= XFS_AG_GEOM_HEALTH_AG_BNOBT;
	if (sick & XFS_HEALTH_AG_CNTBT)
		ageo->ag_health |= XFS_AG_GEOM_HEALTH_AG_CNTBT;
	if (sick & XFS_HEALTH_AG_INOBT)
		ageo->ag_health |= XFS_AG_GEOM_HEALTH_AG_INOBT;
	if (sick & XFS_HEALTH_AG_FINOBT)
		ageo->ag_health |= XFS_AG_GEOM_HEALTH_AG_FINOBT;
	if (sick & XFS_HEALTH_AG_RMAPBT)
		ageo->ag_health |= XFS_AG_GEOM_HEALTH_AG_RMAPBT;
	if (sick & XFS_HEALTH_AG_REFCNTBT)
		ageo->ag_health |= XFS_AG_GEOM_HEALTH_AG_REFCNTBT;
	if (sick & XFS_HEALTH_AG_BAD_INOS)
		ageo->ag_health |= XFS_AG_GEOM_HEALTH_AG_BAD_INOS;
	if (sick & XFS_HEALTH_AG_OFFLINE)
		ageo->ag_health |= XFS_AG_GEOM_HEALTH_AG_OFFLINE;
	xfs_perag_put(pag);
}

/* Fill out bulkstat health info. */
void
xfs_bulkstat_health(
	struct xfs_inode	*ip,
	struct xfs_bstat	*bs)
{
	unsigned int		sick = xfs_inode_measure_sickness(ip);

	bs->bs_health = 0;
	if (sick & XFS_HEALTH_INO_CORE)
		bs->bs_health |= XFS_BS_HEALTH_INODE;
	if (sick & XFS_HEALTH_INO_BMBTD)
		bs->bs_health |= XFS_BS_HEALTH_BMBTD;
	if (sick & XFS_HEALTH_INO_BMBTA)
		bs->bs_health |= XFS_BS_HEALTH_BMBTA;
	if (sick & XFS_HEALTH_INO_BMBTC)
		bs->bs_health |= XFS_BS_HEALTH_BMBTC;
	if (sick & XFS_HEALTH_INO_DIR)
		bs->bs_health |= XFS_BS_HEALTH_DIR;
	if (sick & XFS_HEALTH_INO_XATTR)
		bs->bs_health |= XFS_BS_HEALTH_XATTR;
	if (sick & XFS_HEALTH_INO_SYMLINK)
		bs->bs_health |= XFS_BS_HEALTH_SYMLINK;
	if (sick & XFS_HEALTH_INO_PARENT)
		bs->bs_health |= XFS_BS_HEALTH_PARENT;
}
