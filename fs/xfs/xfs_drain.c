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
#include "xfs_ag.h"
#include "xfs_trace.h"
#include "xfs_rtgroup.h"

/*
 * Use a static key here to reduce the overhead of xfs_drain_drop.  If the
 * compiler supports jump labels, the static branch will be replaced by a nop
 * sled when there are no xfs_drain_wait callers.  Online fsck is currently
 * the only caller, so this is a reasonable tradeoff.
 *
 * Note: Patching the kernel code requires taking the cpu hotplug lock.  Other
 * parts of the kernel allocate memory with that lock held, which means that
 * XFS callers cannot hold any locks that might be used by memory reclaim or
 * writeback when calling the static_branch_{inc,dec} functions.
 */
static DEFINE_STATIC_KEY_FALSE(xfs_drain_waiter_hook);

void
xfs_drain_wait_disable(void)
{
	static_branch_dec(&xfs_drain_waiter_hook);
}

void
xfs_drain_wait_enable(void)
{
	static_branch_inc(&xfs_drain_waiter_hook);
}

void
xfs_drain_init(
	struct xfs_drain	*dr)
{
	atomic_set(&dr->dr_count, 0);
	init_waitqueue_head(&dr->dr_waiters);
}

void
xfs_drain_free(struct xfs_drain	*dr)
{
	ASSERT(atomic_read(&dr->dr_count) == 0);
}

/* Increase the pending intent count. */
static inline void xfs_drain_bump(struct xfs_drain *dr)
{
	atomic_inc(&dr->dr_count);
}

/* Decrease the pending intent count, and wake any waiters, if appropriate. */
static inline void xfs_drain_drop(struct xfs_drain *dr)
{
	if (atomic_dec_and_test(&dr->dr_count) &&
	    static_branch_unlikely(&xfs_drain_waiter_hook))
		wake_up(&dr->dr_waiters);
}

/* Are there work items pending? */
static inline bool xfs_drain_busy(struct xfs_drain *dr)
{
	return atomic_read(&dr->dr_count) > 0;
}

/*
 * Wait for the pending intent count for a drain to hit zero.
 *
 * Callers must not hold any locks that would prevent intents from being
 * finished.
 */
static inline int xfs_drain_wait(struct xfs_drain *dr)
{
	return wait_event_killable(dr->dr_waiters, !xfs_drain_busy(dr));
}

/* Add an item to the pending count. */
void
xfs_perag_bump_intents(
	struct xfs_perag	*pag)
{
	trace_xfs_perag_bump_intents(pag, __return_address);
	xfs_drain_bump(&pag->pag_intents);
}

/* Remove an item from the pending count. */
void
xfs_perag_drop_intents(
	struct xfs_perag	*pag)
{
	trace_xfs_perag_drop_intents(pag, __return_address);
	xfs_drain_drop(&pag->pag_intents);
}

/*
 * Wait for the pending intent count for AG metadata to hit zero.
 * Callers must not hold any AG header buffers.
 */
int
xfs_perag_drain_intents(
	struct xfs_perag	*pag)
{
	trace_xfs_perag_wait_intents(pag, __return_address);
	return xfs_drain_wait(&pag->pag_intents);
}

/* Might someone else be processing intents for this AG? */
bool
xfs_perag_intents_busy(
	struct xfs_perag	*pag)
{
	return xfs_drain_busy(&pag->pag_intents);
}

#ifdef CONFIG_XFS_RT
/* Add an item to the pending count. */
void
xfs_rtgroup_bump_intents(
	struct xfs_rtgroup	*rtg)
{
	trace_xfs_rtgroup_bump_intents(rtg, __return_address);
	xfs_drain_bump(&rtg->rtg_intents);
}

/* Remove an item from the pending count. */
void
xfs_rtgroup_drop_intents(
	struct xfs_rtgroup	*rtg)
{
	trace_xfs_rtgroup_drop_intents(rtg, __return_address);
	xfs_drain_drop(&rtg->rtg_intents);
}

/*
 * Wait for the pending intent count for realtime metadata to hit zero.
 * Callers must not hold any rt metadata inode locks.
 */
int
xfs_rtgroup_drain_intents(
	struct xfs_rtgroup	*rtg)
{
	trace_xfs_rtgroup_wait_intents(rtg, __return_address);
	return xfs_drain_wait(&rtg->rtg_intents);
}

/* Might someone else be processing intents for this rt group? */
bool
xfs_rtgroup_intents_busy(
	struct xfs_rtgroup	*rtg)
{
	return xfs_drain_busy(&rtg->rtg_intents);
}
#endif /* CONFIG_XFS_RT */
