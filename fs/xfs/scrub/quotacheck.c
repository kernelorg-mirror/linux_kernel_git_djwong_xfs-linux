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
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_inode.h"
#include "xfs_quota.h"
#include "xfs_qm.h"
#include "xfs_icache.h"
#include "xfs_bmap_util.h"
#include "xfs_ialloc.h"
#include "xfs_ag.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/repair.h"
#include "scrub/xfarray.h"
#include "scrub/iscan.h"
#include "scrub/quotacheck.h"
#include "scrub/trace.h"

/*
 * Live Quotacheck
 * ===============
 *
 * Quota counters are "summary" metadata, in the sense that they are computed
 * as the summation of the block usage counts for every file on the filesystem.
 * Therefore, we compute the correct icount, bcount, and rtbcount values by
 * creating a shadow quota counter structure and walking every inode.
 *
 * Because we are scanning a live filesystem, it's possible that another thread
 * will try to update the quota counters for an inode that we've already
 * scanned.  This will cause our counts to be incorrect.  Therefore, we hook
 * the live transaction code in two places: (1) when the callers update the
 * per-transaction dqtrx structure to log quota counter updates; and (2) when
 * transaction commit actually logs those updates to the incore dquot.  By
 * shadowing transaction updates in this manner, live quotacheck can ensure
 * by locking the dquot and the shadow structure that its own copies are not
 * out of date.
 *
 * Note that we use srcu notifier hooks to minimize the overhead when live
 * quotacheck is /not/ running.
 */

/* Track the quota deltas for a dquot in a transaction. */
struct xqcheck_dqtrx {
	struct xfs_dquot	*dqp;
	int64_t			icount_delta;

	int64_t			bcount_delta;
	int64_t			delbcnt_delta;

	int64_t			rtbcount_delta;
	int64_t			delrtb_delta;
};

#define XQCHECK_MAX_NR_DQTRXS	(XFS_QM_TRANS_DQTYPES * XFS_QM_TRANS_MAXDQS)

/*
 * Track the quota deltas for all dquots attached to a transaction if the
 * quota deltas are being applied to an inode that we already scanned.
 */
struct xqcheck_dqacct {
	struct rhash_head	hash;
	uintptr_t		tp;
	struct xqcheck_dqtrx	dqtrx[XQCHECK_MAX_NR_DQTRXS];
	unsigned int		refcount;
};

/* Free a shadow dquot accounting structure. */
static void
xqcheck_dqacct_free(
	void			*ptr,
	void			*arg)
{
	struct xqcheck_dqacct	*dqa = ptr;

	kmem_free(dqa);
}

/* Set us up to scrub quota counters. */
int
xchk_setup_quotacheck(
	struct xfs_scrub	*sc)
{
	if (!XFS_IS_QUOTA_ON(sc->mp))
		return -ENOENT;

	sc->buf = kmem_zalloc(sizeof(struct xqcheck), KM_NOFS | KM_MAYFAIL);
	if (!sc->buf)
		return -ENOMEM;

	return xchk_setup_fs(sc);
}

/* Update an incore dquot counter information from a live update. */
static int
xqcheck_update_incore_counts(
	struct xqcheck		*xqc,
	struct xfarray		*counts,
	xfs_dqid_t		id,
	int64_t			inodes,
	int64_t			nblks,
	int64_t			rtblks)
{
	struct xqcheck_dquot	xcdq;
	int			error;

	error = xfarray_load_sparse(counts, id, &xcdq);
	if (error)
		return error;

	xcdq.icount += inodes;
	xcdq.bcount += nblks;
	xcdq.rtbcount += rtblks;

	error = xfarray_store(counts, id, &xcdq);
	if (error == -EFBIG) {
		/*
		 * EFBIG means we tried to store data at too high a byte offset
		 * in the sparse array.  IOWs, we cannot complete the check and
		 * must notify userspace that the check was incomplete.
		 */
		xchk_set_incomplete(xqc->sc);
		error = -ECANCELED;
	}
	return error;
}

/* Decide if this is the shadow dquot accounting structure for a transaction. */
static int
xqcheck_dqacct_obj_cmpfn(
	struct rhashtable_compare_arg	*arg,
	const void			*obj)
{
	const uintptr_t			*key = arg->key;
	const struct xqcheck_dqacct	*dqa = obj;

	if (dqa->tp != *key)
		return 1;
	return 0;
}

static const struct rhashtable_params xqcheck_dqacct_hash_params = {
	.min_size		= 32,
	.key_len		= sizeof(uintptr_t),
	.key_offset		= offsetof(struct xqcheck_dqacct, tp),
	.head_offset		= offsetof(struct xqcheck_dqacct, hash),
	.automatic_shrinking	= true,
	.obj_cmpfn		= xqcheck_dqacct_obj_cmpfn,
};

/* Find a shadow dqtrx slot for the given dquot. */
STATIC struct xqcheck_dqtrx *
xqcheck_get_dqtrx(
	struct xqcheck_dqacct	*dqa,
	struct xfs_dquot	*dqp)
{
	int			i;

	for (i = 0; i < XQCHECK_MAX_NR_DQTRXS; i++) {
		if (dqa->dqtrx[i].dqp == NULL ||
		    dqa->dqtrx[i].dqp == dqp)
			return &dqa->dqtrx[i];
	}

	return NULL;
}

/*
 * Create and fill out a quota delta tracking structure to shadow the updates
 * going on in the regular quota code.
 */
static int
xqcheck_mod_live_ino_dqtrx(
	struct notifier_block		*nb,
	unsigned long			field,
	void				*data)
{
	struct xfs_mod_ino_dqtrx_params *p = data;
	struct xqcheck			*xqc;
	struct xqcheck_dqacct		*dqa;
	struct xqcheck_dqtrx		*dqtrx;
	int				error;

	xqc = container_of(nb, struct xqcheck, mod_dqtrx_hook);

	/* Skip quota reservation fields. */
	switch (field) {
	case XFS_TRANS_DQ_BCOUNT:
	case XFS_TRANS_DQ_DELBCOUNT:
	case XFS_TRANS_DQ_ICOUNT:
	case XFS_TRANS_DQ_RTBCOUNT:
	case XFS_TRANS_DQ_DELRTBCOUNT:
		break;
	default:
		return NOTIFY_DONE;
	}

	/* Ignore dqtrx updates for quota types we don't care about. */
	switch (xfs_dquot_type(p->dqp)) {
	case XFS_DQTYPE_USER:
		if (!xqc->ucounts)
			return NOTIFY_DONE;
		break;
	case XFS_DQTYPE_GROUP:
		if (!xqc->gcounts)
			return NOTIFY_DONE;
		break;
	case XFS_DQTYPE_PROJ:
		if (!xqc->pcounts)
			return NOTIFY_DONE;
		break;
	default:
		return NOTIFY_DONE;
	}

	/* Skip inodes that haven't been scanned yet. */
	if (!xchk_iscan_want_live_update(&xqc->iscan, p->ip->i_ino))
		goto out_done;

	/* Make a shadow quota accounting tracker for this transaction. */
	mutex_lock(&xqc->lock);
	dqa = rhashtable_lookup_fast(&xqc->shadow_dquot_acct, &p->tp,
			xqcheck_dqacct_hash_params);
	if (!dqa) {
		dqa = kmem_zalloc(sizeof(*dqa), KM_MAYFAIL | KM_NOFS);
		if (!dqa)
			goto fail;

		dqa->tp = (uintptr_t)p->tp;
		error = rhashtable_insert_fast(&xqc->shadow_dquot_acct,
				&dqa->hash, xqcheck_dqacct_hash_params);
		if (error)
			goto fail;
	}

	/* Find the shadow dqtrx (or an empty slot) here. */
	dqtrx = xqcheck_get_dqtrx(dqa, p->dqp);
	if (!dqtrx)
		goto fail;
	if (dqtrx->dqp == NULL) {
		dqtrx->dqp = p->dqp;
		dqa->refcount++;
	}

	/* Update counter */
	switch (field) {
	case XFS_TRANS_DQ_BCOUNT:
		dqtrx->bcount_delta += p->delta;
		break;
	case XFS_TRANS_DQ_DELBCOUNT:
		dqtrx->delbcnt_delta += p->delta;
		break;
	case XFS_TRANS_DQ_ICOUNT:
		dqtrx->icount_delta += p->delta;
		break;
	case XFS_TRANS_DQ_RTBCOUNT:
		dqtrx->rtbcount_delta += p->delta;
		break;
	case XFS_TRANS_DQ_DELRTBCOUNT:
		dqtrx->delrtb_delta += p->delta;
		break;
	}

out_unlock:
	mutex_unlock(&xqc->lock);
out_done:
	return NOTIFY_DONE;
fail:
	xchk_iscan_abort(&xqc->iscan);
	goto out_unlock;
}

/*
 * Apply the transaction quota deltas to our shadow quota accounting info when
 * the regular quota code are doing the same.
 */
static int
xqcheck_apply_live_dqtrx(
	struct notifier_block		*nb,
	unsigned long			arg,
	void				*data)
{
	struct xfs_apply_dqtrx_params	*p = data;
	struct xqcheck			*xqc;
	struct xqcheck_dqacct		*dqa;
	struct xqcheck_dqtrx		*dqtrx;
	struct xfarray			*counts;
	int				error;

	xqc = container_of(nb, struct xqcheck, apply_dqtrx_hook);

	/* Map the dquot type to an incore counter object. */
	switch (xfs_dquot_type(p->dqp)) {
	case XFS_DQTYPE_USER:
		counts = xqc->ucounts;
		break;
	case XFS_DQTYPE_GROUP:
		counts = xqc->gcounts;
		break;
	case XFS_DQTYPE_PROJ:
		counts = xqc->pcounts;
		break;
	default:
		return NOTIFY_DONE;
	}

	if (xchk_iscan_aborted(&xqc->iscan) || counts == NULL)
		goto out_done;

	/*
	 * Find the shadow dqtrx for this transaction and dquot, if any deltas
	 * need to be applied here.
	 */
	mutex_lock(&xqc->lock);
	dqa = rhashtable_lookup_fast(&xqc->shadow_dquot_acct, &p->tp,
			xqcheck_dqacct_hash_params);
	if (!dqa)
		goto out_unlock;
	dqtrx = xqcheck_get_dqtrx(dqa, p->dqp);
	if (!dqtrx || dqtrx->dqp == NULL)
		goto out_unlock;

	/* Update our shadow dquot if we're committing. */
	if (arg == XFS_APPLY_DQTRX_COMMIT) {
		error = xqcheck_update_incore_counts(xqc, counts, p->dqp->q_id,
				dqtrx->icount_delta,
				dqtrx->bcount_delta + dqtrx->delbcnt_delta,
				dqtrx->rtbcount_delta + dqtrx->delrtb_delta);
		if (error)
			goto fail;
	}

	/* Free the shadow accounting structure if that was the last user. */
	dqa->refcount--;
	if (dqa->refcount == 0) {
		error = rhashtable_remove_fast(&xqc->shadow_dquot_acct,
				&dqa->hash, xqcheck_dqacct_hash_params);
		if (error)
			goto fail;
		xqcheck_dqacct_free(dqa, NULL);
	}

out_unlock:
	mutex_unlock(&xqc->lock);
out_done:
	return NOTIFY_DONE;
fail:
	xchk_iscan_abort(&xqc->iscan);
	goto out_unlock;
}

/* Record this inode's quota usage in our shadow quota counter data. */
STATIC int
xqcheck_inode(
	struct xqcheck		*xqc,
	struct xfs_inode	*ip)
{
	struct xfs_trans	*tp = xqc->sc->tp;
	xfs_filblks_t		nblks, rtblks;
	uint			ilock_flags = 0;
	xfs_dqid_t		id;
	int			error;

	if (xfs_is_quota_inode(&tp->t_mountp->m_sb, ip->i_ino)) {
		/*
		 * Quota inode blocks are never counted towards quota, so we
		 * do not need to take the lock.
		 */
		xchk_iscan_mark_visited(&xqc->iscan, ip);
		return 0;
	}

	/* Figure out the data / rt device block counts. */
	xfs_ilock(ip, XFS_IOLOCK_SHARED | XFS_MMAPLOCK_SHARED);
	ilock_flags = xfs_ilock_data_map_shared(ip);
	if (XFS_IS_REALTIME_INODE(ip)) {
		error = xfs_iread_extents(tp, ip, XFS_DATA_FORK);
		if (error)
			goto out_ilock;
	}
	xfs_inode_count_blocks(tp, ip, &nblks, &rtblks);

	if (xchk_iscan_aborted(&xqc->iscan)) {
		error = -ECANCELED;
		goto out_ilock;
	}

	/* Update the shadow dquot counters. */
	mutex_lock(&xqc->lock);
	id = xfs_qm_id_for_quotatype(ip, XFS_DQTYPE_USER);
	if (xqc->ucounts) {
		error = xqcheck_update_incore_counts(xqc, xqc->ucounts, id, 1,
				nblks, rtblks);
		if (error)
			goto out_incomplete;
	}

	id = xfs_qm_id_for_quotatype(ip, XFS_DQTYPE_GROUP);
	if (xqc->gcounts) {
		error = xqcheck_update_incore_counts(xqc, xqc->gcounts, id, 1,
				nblks, rtblks);
		if (error)
			goto out_incomplete;
	}

	id = xfs_qm_id_for_quotatype(ip, XFS_DQTYPE_PROJ);
	if (xqc->pcounts) {
		error = xqcheck_update_incore_counts(xqc, xqc->pcounts, id, 1,
				nblks, rtblks);
		if (error)
			goto out_incomplete;
	}
	mutex_unlock(&xqc->lock);

	xchk_iscan_mark_visited(&xqc->iscan, ip);
	goto out_ilock;

out_incomplete:
	mutex_unlock(&xqc->lock);
	xchk_set_incomplete(xqc->sc);
	xchk_iscan_abort(&xqc->iscan);
out_ilock:
	xfs_iunlock(ip, XFS_IOLOCK_SHARED | XFS_MMAPLOCK_SHARED | ilock_flags);
	return error;
}

/* Walk all the allocated inodes and run a quota scan on them. */
STATIC int
xqcheck_collect_counts(
	struct xqcheck		*xqc)
{
	struct xfs_scrub	*sc = xqc->sc;
	struct xchk_iscan	*iscan = &xqc->iscan;
	int			error;

	/*
	 * Set up for a potentially lengthy filesystem scan by reducing our
	 * transaction resource usage for the duration.  Specifically:
	 *
	 * Cancel the transaction to release the log grant space while we scan
	 * the filesystem.
	 *
	 * Create a new empty transaction to eliminate the possibility of the
	 * inode scan deadlocking on cyclical metadata.
	 *
	 * We pass the empty transaction to the file scanning function to avoid
	 * repeatedly cycling empty transactions.  This can be done without
	 * risk of deadlock between sb_internal and the IOLOCK (we take the
	 * IOLOCK to quiesce the file before scanning) because empty
	 * transactions do not take sb_internal.
	 */
	xchk_trans_cancel(sc);
	error = xchk_trans_alloc_empty(sc);
	if (error)
		return error;

	while ((error = xchk_iscan_advance(sc, iscan)) == 1) {
		struct xfs_inode	*ip;

		error = xchk_iscan_iget(sc, iscan, &ip);
		if (error == -EAGAIN)
			continue;
		if (error)
			break;

		error = xqcheck_inode(xqc, ip);
		xchk_irele(sc, ip);
		if (error)
			break;

		if (xchk_should_terminate(sc, &error))
			break;
	}

	if (error == -ECANCELED)
		xchk_set_incomplete(sc);
	if (error)
		return error;

	/*
	 * Switch out for a real transaction in preparation for building a new
	 * tree.
	 */
	xchk_trans_cancel(sc);
	return xchk_setup_fs(sc);
}

/*
 * Check the dquot data against what we observed.  Caller must hold the dquot
 * lock.
 */
STATIC int
xqcheck_compare_dquot(
	struct xfs_dquot	*dqp,
	xfs_dqtype_t		dqtype,
	void			*priv)
{
	struct xqcheck_dquot	xcdq;
	struct xqcheck		*xqc = priv;
	struct xfarray		*counts = xqcheck_counters_for(xqc, dqtype);
	int			error;

	if (xchk_iscan_aborted(&xqc->iscan)) {
		xchk_set_incomplete(xqc->sc);
		return -ECANCELED;
	}

	mutex_lock(&xqc->lock);
	error = xfarray_load_sparse(counts, dqp->q_id, &xcdq);
	if (error)
		goto out_unlock;

	if (xcdq.icount != dqp->q_ino.count)
		xchk_qcheck_set_corrupt(xqc->sc, dqtype, dqp->q_id);

	if (xcdq.bcount != dqp->q_blk.count)
		xchk_qcheck_set_corrupt(xqc->sc, dqtype, dqp->q_id);

	if (xcdq.rtbcount != dqp->q_rtb.count)
		xchk_qcheck_set_corrupt(xqc->sc, dqtype, dqp->q_id);

	xcdq.flags |= XQCHECK_DQUOT_COMPARE_SCANNED;
	error = xfarray_store(counts, dqp->q_id, &xcdq);
	if (error == -EFBIG) {
		/*
		 * EFBIG means we tried to store data at too high a byte offset
		 * in the sparse array.  IOWs, we cannot complete the check and
		 * must notify userspace that the check was incomplete.
		 */
		xchk_set_incomplete(xqc->sc);
		error = -ECANCELED;
	}
	mutex_unlock(&xqc->lock);
	if (error)
		return error;

	if (xqc->sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return -ECANCELED;

	return 0;
out_unlock:
	mutex_unlock(&xqc->lock);
	return error;
}

/*
 * Walk all the observed dquots, and make sure there's a matching incore
 * dquot and that its counts match ours.
 */
STATIC int
xqcheck_walk_observations(
	struct xqcheck		*xqc,
	xfs_dqtype_t		dqtype)
{
	struct xqcheck_dquot	xcdq;
	struct xfs_dquot	*dqp;
	struct xfarray		*counts = xqcheck_counters_for(xqc, dqtype);
	uint64_t		nr = 0;
	int			error;

	mutex_lock(&xqc->lock);
	while ((error = xfarray_iter(counts, &nr, &xcdq)) == 1) {
		xfs_dqid_t	id = nr - 1;

		if (xcdq.flags & XQCHECK_DQUOT_COMPARE_SCANNED)
			continue;

		mutex_unlock(&xqc->lock);

		error = xfs_qm_dqget(xqc->sc->mp, id, dqtype, false, &dqp);
		if (error == -ENOENT) {
			xchk_qcheck_set_corrupt(xqc->sc, dqtype, id);
			return 0;
		}
		if (error)
			return error;

		error = xqcheck_compare_dquot(dqp, dqtype, xqc);
		xfs_qm_dqput(dqp);
		if (error)
			return error;

		if (xchk_should_terminate(xqc->sc, &error))
			return error;

		mutex_lock(&xqc->lock);
	}
	mutex_unlock(&xqc->lock);

	return error;
}

/* Compare the quota counters we observed against the live dquots. */
STATIC int
xqcheck_compare_dqtype(
	struct xqcheck		*xqc,
	xfs_dqtype_t		dqtype)
{
	struct xfs_scrub	*sc = xqc->sc;
	int			error;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return 0;

	/* If the quota CHKD flag is cleared, we need to repair this quota. */
	if (!(xfs_quota_chkd_flag(dqtype) & sc->mp->m_qflags)) {
		xchk_qcheck_set_corrupt(xqc->sc, dqtype, 0);
		return 0;
	}

	/* Compare what we observed against the actual dquots. */
	error = xfs_qm_dqiterate(sc->mp, dqtype, xqcheck_compare_dquot, xqc);
	if (error)
		return error;

	/* Walk all the observed dquots and compare to the incore ones. */
	return xqcheck_walk_observations(xqc, dqtype);
}

/* Tear down everything associated with a quotacheck. */
static void
xqcheck_teardown_scan(
	struct xqcheck		*xqc)
{
	struct xfs_quotainfo	*qi = xqc->sc->mp->m_quotainfo;

	/* Discourage any hook functions that might be running. */
	xchk_iscan_abort(&xqc->iscan);

	/*
	 * As noted above, the apply hook is responsible for cleaning up the
	 * shadow dquot accounting data when a transaction completes.  The mod
	 * hook must be removed before the apply hook so that we don't
	 * mistakenly leave an active shadow account for the mod hook to get
	 * its hands on.  No hooks should be running after these functions
	 * return.
	 */
	xfs_hook_del(&qi->qi_mod_ino_dqtrx_hooks, &xqc->mod_dqtrx_hook);
	xfs_hook_del(&qi->qi_apply_dqtrx_hooks, &xqc->apply_dqtrx_hook);

	if (xqc->shadow_dquot_acct.key_len) {
		rhashtable_free_and_destroy(&xqc->shadow_dquot_acct,
				xqcheck_dqacct_free, NULL);
		xqc->shadow_dquot_acct.key_len = 0;
	}

	if (xqc->pcounts) {
		xfarray_destroy(xqc->pcounts);
		xqc->pcounts = NULL;
	}

	if (xqc->gcounts) {
		xfarray_destroy(xqc->gcounts);
		xqc->gcounts = NULL;
	}

	if (xqc->ucounts) {
		xfarray_destroy(xqc->ucounts);
		xqc->ucounts = NULL;
	}

	xchk_iscan_finish(&xqc->iscan);
	mutex_destroy(&xqc->lock);
	xqc->sc = NULL;
}

/*
 * Scan all inodes in the entire filesystem to generate quota counter data.
 * If the scan is successful, the quota data will be left alive for a repair.
 * If any error occurs, we'll tear everything down.
 */
STATIC int
xqcheck_setup_scan(
	struct xfs_scrub	*sc,
	struct xqcheck		*xqc)
{
	struct xfs_quotainfo	*qi = sc->mp->m_quotainfo;
	int			error;

	ASSERT(xqc->sc == NULL);
	xqc->sc = sc;

	mutex_init(&xqc->lock);
	xqc->iscan.iget_tries = 20;
	xqc->iscan.iget_retry_delay = HZ / 10;
	xchk_iscan_start(&xqc->iscan);

	error = -ENOMEM;
	if (xfs_this_quota_on(sc->mp, XFS_DQTYPE_USER)) {
		error = xfarray_create(sc->mp, "user dquots",
				sizeof(struct xqcheck_dquot), &xqc->ucounts);
		if (error)
			goto out_teardown;
	}

	if (xfs_this_quota_on(sc->mp, XFS_DQTYPE_GROUP)) {
		error = xfarray_create(sc->mp, "group dquots",
				sizeof(struct xqcheck_dquot), &xqc->gcounts);
		if (error)
			goto out_teardown;
	}

	if (xfs_this_quota_on(sc->mp, XFS_DQTYPE_PROJ)) {
		error = xfarray_create(sc->mp, "project dquots",
				sizeof(struct xqcheck_dquot), &xqc->pcounts);
		if (error)
			goto out_teardown;
	}

	/*
	 * Set up hash table to map transactions to our internal shadow dqtrx
	 * structures.
	 */
	error = rhashtable_init(&xqc->shadow_dquot_acct,
			&xqcheck_dqacct_hash_params);
	if (error)
		goto out_teardown;

	/*
	 * Hook into the quota code.  The hook only triggers for inodes that
	 * were already scanned, and the scanner thread takes each inode's
	 * ILOCK, which means that any in-progress inode updates will finish
	 * before we can scan the inode.
	 *
	 * The apply hook (which removes the shadow dquot accounting struct)
	 * must be installed before the mod hook so that we never fail to catch
	 * the end of a quota update sequence and leave stale shadow data.
	 */
	error = xfs_hook_add(&qi->qi_apply_dqtrx_hooks, &xqc->apply_dqtrx_hook,
			xqcheck_apply_live_dqtrx);
	if (error)
		goto out_teardown;
	error = xfs_hook_add(&qi->qi_mod_ino_dqtrx_hooks, &xqc->mod_dqtrx_hook,
			xqcheck_mod_live_ino_dqtrx);
	if (error)
		goto out_teardown;

	/* Use deferred cleanup to pass the quota count data to repair. */
	sc->buf_cleanup = (void (*)(void *))xqcheck_teardown_scan;
	return 0;

out_teardown:
	xqcheck_teardown_scan(xqc);
	return error;
}

/* Scrub all counters for a given quota type. */
int
xchk_quotacheck(
	struct xfs_scrub	*sc)
{
	struct xqcheck		*xqc = sc->buf;
	int			error = 0;

	/* Check quota counters on the live filesystem. */
	error = xqcheck_setup_scan(sc, xqc);
	if (error)
		return error;

	/* Walk all inodes, picking up quota information. */
	error = xqcheck_collect_counts(xqc);
	if (!xchk_xref_process_error(sc, 0, 0, &error))
		return error;

	/* Compare quota counters. */
	if (xqc->ucounts) {
		error = xqcheck_compare_dqtype(xqc, XFS_DQTYPE_USER);
		if (!xchk_xref_process_error(sc, 0, 0, &error))
			return error;
	}
	if (xqc->gcounts) {
		error = xqcheck_compare_dqtype(xqc, XFS_DQTYPE_GROUP);
		if (!xchk_xref_process_error(sc, 0, 0, &error))
			return error;
	}
	if (xqc->pcounts) {
		error = xqcheck_compare_dqtype(xqc, XFS_DQTYPE_PROJ);
		if (!xchk_xref_process_error(sc, 0, 0, &error))
			return error;
	}

	return 0;
}
