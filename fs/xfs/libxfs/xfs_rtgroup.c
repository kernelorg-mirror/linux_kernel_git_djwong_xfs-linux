// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_rmap_btree.h"
#include "xfs_alloc.h"
#include "xfs_ialloc.h"
#include "xfs_rmap.h"
#include "xfs_ag.h"
#include "xfs_ag_resv.h"
#include "xfs_health.h"
#include "xfs_error.h"
#include "xfs_bmap.h"
#include "xfs_defer.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_trace.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_rtgroup.h"
#include "xfs_rtbitmap.h"

/*
 * Passive reference counting access wrappers to the rtgroup structures.  If
 * the rtgroup structure is to be freed, the freeing code is responsible for
 * cleaning up objects with passive references before freeing the structure.
 */
struct xfs_rtgroup *
xfs_rtgroup_get(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	struct xfs_rtgroup	*rtg;

	rcu_read_lock();
	rtg = radix_tree_lookup(&mp->m_rtgroup_tree, rgno);
	if (rtg) {
		trace_xfs_rtgroup_get(rtg, _RET_IP_);
		ASSERT(atomic_read(&rtg->rtg_ref) >= 0);
		atomic_inc(&rtg->rtg_ref);
	}
	rcu_read_unlock();
	return rtg;
}

/* Get a passive reference to the given rtgroup. */
struct xfs_rtgroup *
xfs_rtgroup_hold(
	struct xfs_rtgroup	*rtg)
{
	ASSERT(atomic_read(&rtg->rtg_ref) > 0 ||
	       atomic_read(&rtg->rtg_active_ref) > 0);

	trace_xfs_rtgroup_hold(rtg, _RET_IP_);
	atomic_inc(&rtg->rtg_ref);
	return rtg;
}

void
xfs_rtgroup_put(
	struct xfs_rtgroup	*rtg)
{
	trace_xfs_rtgroup_put(rtg, _RET_IP_);
	ASSERT(atomic_read(&rtg->rtg_ref) > 0);
	atomic_dec(&rtg->rtg_ref);
}

/*
 * Active references for rtgroup structures. This is for short term access to
 * the rtgroup structures for walking trees or accessing state. If an rtgroup
 * is being shrunk or is offline, then this will fail to find that group and
 * return NULL instead.
 */
struct xfs_rtgroup *
xfs_rtgroup_grab(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno)
{
	struct xfs_rtgroup	*rtg;

	rcu_read_lock();
	rtg = radix_tree_lookup(&mp->m_rtgroup_tree, agno);
	if (rtg) {
		trace_xfs_rtgroup_grab(rtg, _RET_IP_);
		if (!atomic_inc_not_zero(&rtg->rtg_active_ref))
			rtg = NULL;
	}
	rcu_read_unlock();
	return rtg;
}

void
xfs_rtgroup_rele(
	struct xfs_rtgroup	*rtg)
{
	trace_xfs_rtgroup_rele(rtg, _RET_IP_);
	if (atomic_dec_and_test(&rtg->rtg_active_ref))
		wake_up(&rtg->rtg_active_wq);
}

int
xfs_rtgroup_alloc(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	struct xfs_rtgroup	*rtg;
	int			error;

	rtg = kzalloc(sizeof(struct xfs_rtgroup), GFP_KERNEL);
	if (!rtg)
		return -ENOMEM;
	rtg->rtg_rgno = rgno;
	rtg->rtg_mount = mp;

	error = radix_tree_preload(GFP_NOFS);
	if (error)
		goto out_free_rtg;

	spin_lock(&mp->m_rtgroup_lock);
	if (radix_tree_insert(&mp->m_rtgroup_tree, rgno, rtg)) {
		spin_unlock(&mp->m_rtgroup_lock);
		radix_tree_preload_end();
		error = -EEXIST;
		goto out_free_rtg;
	}
	spin_unlock(&mp->m_rtgroup_lock);
	radix_tree_preload_end();

#ifdef __KERNEL__
	/* Place kernel structure only init below this point. */
	spin_lock_init(&rtg->rtg_state_lock);
	init_waitqueue_head(&rtg->rtg_active_wq);
#endif /* __KERNEL__ */

	/* Active ref owned by mount indicates rtgroup is online. */
	atomic_set(&rtg->rtg_active_ref, 1);
	return 0;

out_free_rtg:
	kfree(rtg);
	return error;
}

void
xfs_rtgroup_free(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	struct xfs_rtgroup	*rtg;

	spin_lock(&mp->m_rtgroup_lock);
	rtg = radix_tree_delete(&mp->m_rtgroup_tree, rgno);
	spin_unlock(&mp->m_rtgroup_lock);

	if (!rtg) /* can happen when growfs fails */
		return;

	XFS_IS_CORRUPT(mp, atomic_read(&rtg->rtg_ref) != 0);

	/* drop the mount's active reference */
	xfs_rtgroup_rele(rtg);
	XFS_IS_CORRUPT(mp, atomic_read(&rtg->rtg_active_ref) != 0);

	kfree_rcu_mightsleep(rtg);
}

/*
 * Free up the rtgroup resources associated with the mount structure.
 */
void
xfs_free_rtgroups(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgcount)
{
	xfs_rgnumber_t		rgno;

	for (rgno = 0; rgno < rgcount; rgno++)
		xfs_rtgroup_free(mp, rgno);
}

/* Compute the number of rt extents in this realtime group. */
xfs_rtxnum_t
xfs_rtgroup_extents(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	xfs_rgnumber_t		rgcount = mp->m_sb.sb_rgcount;

	ASSERT(rgno < rgcount);
	if (rgno == rgcount - 1)
		return mp->m_sb.sb_rextents -
			((xfs_rtxnum_t)rgno * mp->m_sb.sb_rgextents);

	ASSERT(xfs_has_rtgroups(mp));
	return mp->m_sb.sb_rgextents;
}
