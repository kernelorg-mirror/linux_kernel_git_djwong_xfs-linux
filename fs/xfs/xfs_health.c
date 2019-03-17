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
