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
#include "xfs_ag.h"
#include "xfs_trace.h"

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
static inline void xfs_drain_grab(struct xfs_drain *dr)
{
	atomic_inc(&dr->dr_count);
}

static inline bool has_waiters(struct wait_queue_head *wq_head)
{
	/*
	 * This memory barrier is paired with the one in set_current_state on
	 * the waiting side.
	 */
	smp_mb__after_atomic();
	return waitqueue_active(wq_head);
}

/* Decrease the pending intent count, and wake any waiters, if appropriate. */
static inline void xfs_drain_rele(struct xfs_drain *dr)
{
	if (atomic_dec_and_test(&dr->dr_count) &&
	    has_waiters(&dr->dr_waiters))
		wake_up(&dr->dr_waiters);
}

/* Are there intents pending? */
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

/*
 * Declare an intent to update AG metadata.  Other threads that need exclusive
 * access can decide to back off if they see declared intentions.
 */
void
xfs_perag_intent_hold(
	struct xfs_perag	*pag)
{
	trace_xfs_perag_intent_hold(pag, __return_address);
	xfs_drain_grab(&pag->pag_intents);
}

/* Release our intent to update this AG's metadata. */
void
xfs_perag_intent_rele(
	struct xfs_perag	*pag)
{
	trace_xfs_perag_intent_rele(pag, __return_address);
	xfs_drain_rele(&pag->pag_intents);
}

/*
 * Wait for the intent update count for this AG to hit zero.
 * Callers must not hold any AG header buffers.
 */
int
xfs_perag_drain_intents(
	struct xfs_perag	*pag)
{
	trace_xfs_perag_wait_intents(pag, __return_address);
	return xfs_drain_wait(&pag->pag_intents);
}

/* Has anyone declared an intent to update this AG? */
bool
xfs_perag_intents_busy(
	struct xfs_perag	*pag)
{
	return xfs_drain_busy(&pag->pag_intents);
}
