// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2003 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_quota.h"
#include "xfs_trans.h"
#include "xfs_buf_item.h"
#include "xfs_trans_priv.h"
#include "xfs_qm.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_log_recover.h"

static inline struct xfs_dq_logitem *DQUOT_ITEM(struct xfs_log_item *lip)
{
	return container_of(lip, struct xfs_dq_logitem, qli_item);
}

/*
 * returns the number of iovecs needed to log the given dquot item.
 */
STATIC void
xfs_qm_dquot_logitem_size(
	struct xfs_log_item	*lip,
	int			*nvecs,
	int			*nbytes)
{
	*nvecs += 2;
	*nbytes += sizeof(struct xfs_dq_logformat) +
		   sizeof(struct xfs_disk_dquot);
}

/*
 * fills in the vector of log iovecs for the given dquot log item.
 */
STATIC void
xfs_qm_dquot_logitem_format(
	struct xfs_log_item	*lip,
	struct xfs_log_vec	*lv)
{
	struct xfs_dq_logitem	*qlip = DQUOT_ITEM(lip);
	struct xfs_log_iovec	*vecp = NULL;
	struct xfs_dq_logformat	*qlf;

	qlf = xlog_prepare_iovec(lv, &vecp, XLOG_REG_TYPE_QFORMAT);
	qlf->qlf_type = XFS_LI_DQUOT;
	qlf->qlf_size = 2;
	qlf->qlf_id = be32_to_cpu(qlip->qli_dquot->q_core.d_id);
	qlf->qlf_blkno = qlip->qli_dquot->q_blkno;
	qlf->qlf_len = 1;
	qlf->qlf_boffset = qlip->qli_dquot->q_bufoffset;
	xlog_finish_iovec(lv, vecp, sizeof(struct xfs_dq_logformat));

	xlog_copy_iovec(lv, &vecp, XLOG_REG_TYPE_DQUOT,
			&qlip->qli_dquot->q_core,
			sizeof(struct xfs_disk_dquot));
}

/*
 * Increment the pin count of the given dquot.
 */
STATIC void
xfs_qm_dquot_logitem_pin(
	struct xfs_log_item	*lip)
{
	struct xfs_dquot	*dqp = DQUOT_ITEM(lip)->qli_dquot;

	ASSERT(XFS_DQ_IS_LOCKED(dqp));
	atomic_inc(&dqp->q_pincount);
}

/*
 * Decrement the pin count of the given dquot, and wake up
 * anyone in xfs_dqwait_unpin() if the count goes to 0.	 The
 * dquot must have been previously pinned with a call to
 * xfs_qm_dquot_logitem_pin().
 */
STATIC void
xfs_qm_dquot_logitem_unpin(
	struct xfs_log_item	*lip,
	int			remove)
{
	struct xfs_dquot	*dqp = DQUOT_ITEM(lip)->qli_dquot;

	ASSERT(atomic_read(&dqp->q_pincount) > 0);
	if (atomic_dec_and_test(&dqp->q_pincount))
		wake_up(&dqp->q_pinwait);
}

/*
 * This is called to wait for the given dquot to be unpinned.
 * Most of these pin/unpin routines are plagiarized from inode code.
 */
void
xfs_qm_dqunpin_wait(
	struct xfs_dquot	*dqp)
{
	ASSERT(XFS_DQ_IS_LOCKED(dqp));
	if (atomic_read(&dqp->q_pincount) == 0)
		return;

	/*
	 * Give the log a push so we don't wait here too long.
	 */
	xfs_log_force(dqp->q_mount, 0);
	wait_event(dqp->q_pinwait, (atomic_read(&dqp->q_pincount) == 0));
}

/*
 * Callback used to mark a buffer with XFS_LI_FAILED when items in the buffer
 * have been failed during writeback
 *
 * this informs the AIL that the dquot is already flush locked on the next push,
 * and acquires a hold on the buffer to ensure that it isn't reclaimed before
 * dirty data makes it to disk.
 */
STATIC void
xfs_dquot_item_error(
	struct xfs_log_item	*lip,
	struct xfs_buf		*bp)
{
	ASSERT(!completion_done(&DQUOT_ITEM(lip)->qli_dquot->q_flush));
	xfs_set_li_failed(lip, bp);
}

STATIC uint
xfs_qm_dquot_logitem_push(
	struct xfs_log_item	*lip,
	struct list_head	*buffer_list)
		__releases(&lip->li_ailp->ail_lock)
		__acquires(&lip->li_ailp->ail_lock)
{
	struct xfs_dquot	*dqp = DQUOT_ITEM(lip)->qli_dquot;
	struct xfs_buf		*bp = lip->li_buf;
	uint			rval = XFS_ITEM_SUCCESS;
	int			error;

	if (atomic_read(&dqp->q_pincount) > 0)
		return XFS_ITEM_PINNED;

	/*
	 * The buffer containing this item failed to be written back
	 * previously. Resubmit the buffer for IO
	 */
	if (test_bit(XFS_LI_FAILED, &lip->li_flags)) {
		if (!xfs_buf_trylock(bp))
			return XFS_ITEM_LOCKED;

		if (!xfs_buf_resubmit_failed_buffers(bp, buffer_list))
			rval = XFS_ITEM_FLUSHING;

		xfs_buf_unlock(bp);
		return rval;
	}

	if (!xfs_dqlock_nowait(dqp))
		return XFS_ITEM_LOCKED;

	/*
	 * Re-check the pincount now that we stabilized the value by
	 * taking the quota lock.
	 */
	if (atomic_read(&dqp->q_pincount) > 0) {
		rval = XFS_ITEM_PINNED;
		goto out_unlock;
	}

	/*
	 * Someone else is already flushing the dquot.  Nothing we can do
	 * here but wait for the flush to finish and remove the item from
	 * the AIL.
	 */
	if (!xfs_dqflock_nowait(dqp)) {
		rval = XFS_ITEM_FLUSHING;
		goto out_unlock;
	}

	spin_unlock(&lip->li_ailp->ail_lock);

	error = xfs_qm_dqflush(dqp, &bp);
	if (!error) {
		if (!xfs_buf_delwri_queue(bp, buffer_list))
			rval = XFS_ITEM_FLUSHING;
		xfs_buf_relse(bp);
	} else if (error == -EAGAIN)
		rval = XFS_ITEM_LOCKED;

	spin_lock(&lip->li_ailp->ail_lock);
out_unlock:
	xfs_dqunlock(dqp);
	return rval;
}

STATIC void
xfs_qm_dquot_logitem_release(
	struct xfs_log_item	*lip)
{
	struct xfs_dquot	*dqp = DQUOT_ITEM(lip)->qli_dquot;

	ASSERT(XFS_DQ_IS_LOCKED(dqp));

	/*
	 * dquots are never 'held' from getting unlocked at the end of
	 * a transaction.  Their locking and unlocking is hidden inside the
	 * transaction layer, within trans_commit. Hence, no LI_HOLD flag
	 * for the logitem.
	 */
	xfs_dqunlock(dqp);
}

STATIC void
xfs_qm_dquot_logitem_committing(
	struct xfs_log_item	*lip,
	xfs_lsn_t		commit_lsn)
{
	return xfs_qm_dquot_logitem_release(lip);
}

static const struct xfs_item_ops xfs_dquot_item_ops = {
	.iop_size	= xfs_qm_dquot_logitem_size,
	.iop_format	= xfs_qm_dquot_logitem_format,
	.iop_pin	= xfs_qm_dquot_logitem_pin,
	.iop_unpin	= xfs_qm_dquot_logitem_unpin,
	.iop_release	= xfs_qm_dquot_logitem_release,
	.iop_committing	= xfs_qm_dquot_logitem_committing,
	.iop_push	= xfs_qm_dquot_logitem_push,
	.iop_error	= xfs_dquot_item_error
};

/*
 * Initialize the dquot log item for a newly allocated dquot.
 * The dquot isn't locked at this point, but it isn't on any of the lists
 * either, so we don't care.
 */
void
xfs_qm_dquot_logitem_init(
	struct xfs_dquot	*dqp)
{
	struct xfs_dq_logitem	*lp = &dqp->q_logitem;

	xfs_log_item_init(dqp->q_mount, &lp->qli_item, XFS_LI_DQUOT,
					&xfs_dquot_item_ops);
	lp->qli_dquot = dqp;
}

/*------------------  QUOTAOFF LOG ITEMS  -------------------*/

static inline struct xfs_qoff_logitem *QOFF_ITEM(struct xfs_log_item *lip)
{
	return container_of(lip, struct xfs_qoff_logitem, qql_item);
}


/*
 * This returns the number of iovecs needed to log the given quotaoff item.
 * We only need 1 iovec for an quotaoff item.  It just logs the
 * quotaoff_log_format structure.
 */
STATIC void
xfs_qm_qoff_logitem_size(
	struct xfs_log_item	*lip,
	int			*nvecs,
	int			*nbytes)
{
	*nvecs += 1;
	*nbytes += sizeof(struct xfs_qoff_logitem);
}

STATIC void
xfs_qm_qoff_logitem_format(
	struct xfs_log_item	*lip,
	struct xfs_log_vec	*lv)
{
	struct xfs_qoff_logitem	*qflip = QOFF_ITEM(lip);
	struct xfs_log_iovec	*vecp = NULL;
	struct xfs_qoff_logformat *qlf;

	qlf = xlog_prepare_iovec(lv, &vecp, XLOG_REG_TYPE_QUOTAOFF);
	qlf->qf_type = XFS_LI_QUOTAOFF;
	qlf->qf_size = 1;
	qlf->qf_flags = qflip->qql_flags;
	xlog_finish_iovec(lv, vecp, sizeof(struct xfs_qoff_logitem));
}

/*
 * There isn't much you can do to push a quotaoff item.  It is simply
 * stuck waiting for the log to be flushed to disk.
 */
STATIC uint
xfs_qm_qoff_logitem_push(
	struct xfs_log_item	*lip,
	struct list_head	*buffer_list)
{
	return XFS_ITEM_LOCKED;
}

STATIC xfs_lsn_t
xfs_qm_qoffend_logitem_committed(
	struct xfs_log_item	*lip,
	xfs_lsn_t		lsn)
{
	struct xfs_qoff_logitem	*qfe = QOFF_ITEM(lip);
	struct xfs_qoff_logitem	*qfs = qfe->qql_start_lip;

	xfs_qm_qoff_logitem_relse(qfs);

	kmem_free(lip->li_lv_shadow);
	kmem_free(qfe);
	return (xfs_lsn_t)-1;
}

STATIC void
xfs_qm_qoff_logitem_release(
	struct xfs_log_item	*lip)
{
	struct xfs_qoff_logitem	*qoff = QOFF_ITEM(lip);

	if (test_bit(XFS_LI_ABORTED, &lip->li_flags)) {
		if (qoff->qql_start_lip)
			xfs_qm_qoff_logitem_relse(qoff->qql_start_lip);
		xfs_qm_qoff_logitem_relse(qoff);
	}
}

static const struct xfs_item_ops xfs_qm_qoffend_logitem_ops = {
	.iop_size	= xfs_qm_qoff_logitem_size,
	.iop_format	= xfs_qm_qoff_logitem_format,
	.iop_committed	= xfs_qm_qoffend_logitem_committed,
	.iop_push	= xfs_qm_qoff_logitem_push,
	.iop_release	= xfs_qm_qoff_logitem_release,
};

static const struct xfs_item_ops xfs_qm_qoff_logitem_ops = {
	.iop_size	= xfs_qm_qoff_logitem_size,
	.iop_format	= xfs_qm_qoff_logitem_format,
	.iop_push	= xfs_qm_qoff_logitem_push,
	.iop_release	= xfs_qm_qoff_logitem_release,
};

/*
 * Delete the quotaoff intent from the AIL and free it. On success,
 * this should only be called for the start item. It can be used for
 * either on shutdown or abort.
 */
void
xfs_qm_qoff_logitem_relse(
	struct xfs_qoff_logitem	*qoff)
{
	struct xfs_log_item	*lip = &qoff->qql_item;

	ASSERT(test_bit(XFS_LI_IN_AIL, &lip->li_flags) ||
	       test_bit(XFS_LI_ABORTED, &lip->li_flags) ||
	       XFS_FORCED_SHUTDOWN(lip->li_mountp));
	xfs_trans_ail_remove(lip, SHUTDOWN_LOG_IO_ERROR);
	kmem_free(lip->li_lv_shadow);
	kmem_free(qoff);
}

/*
 * Allocate and initialize an quotaoff item of the correct quota type(s).
 */
struct xfs_qoff_logitem *
xfs_qm_qoff_logitem_init(
	struct xfs_mount	*mp,
	struct xfs_qoff_logitem	*start,
	uint			flags)
{
	struct xfs_qoff_logitem	*qf;

	qf = kmem_zalloc(sizeof(struct xfs_qoff_logitem), 0);

	xfs_log_item_init(mp, &qf->qql_item, XFS_LI_QUOTAOFF, start ?
			&xfs_qm_qoffend_logitem_ops : &xfs_qm_qoff_logitem_ops);
	qf->qql_item.li_mountp = mp;
	qf->qql_start_lip = start;
	qf->qql_flags = flags;
	return qf;
}

STATIC void
xlog_recover_dquot_ra_pass2(
	struct xlog			*log,
	struct xlog_recover_item	*item)
{
	struct xfs_mount	*mp = log->l_mp;
	struct xfs_disk_dquot	*recddq;
	struct xfs_dq_logformat	*dq_f;
	uint			type;
	int			len;


	if (mp->m_qflags == 0)
		return;

	recddq = item->ri_buf[1].i_addr;
	if (recddq == NULL)
		return;
	if (item->ri_buf[1].i_len < sizeof(struct xfs_disk_dquot))
		return;

	type = recddq->d_flags & (XFS_DQ_USER | XFS_DQ_PROJ | XFS_DQ_GROUP);
	ASSERT(type);
	if (log->l_quotaoffs_flag & type)
		return;

	dq_f = item->ri_buf[0].i_addr;
	ASSERT(dq_f);
	ASSERT(dq_f->qlf_len == 1);

	len = XFS_FSB_TO_BB(mp, dq_f->qlf_len);
	if (xlog_peek_buffer_cancelled(log, dq_f->qlf_blkno, len, 0))
		return;

	xfs_buf_readahead(mp->m_ddev_targp, dq_f->qlf_blkno, len,
			  &xfs_dquot_buf_ra_ops);
}

/*
 * Recover a dquot record
 */
STATIC int
xlog_recover_dquot_pass2(
	struct xlog			*log,
	struct list_head		*buffer_list,
	struct xlog_recover_item	*item,
	xfs_lsn_t			current_lsn)
{
	xfs_mount_t		*mp = log->l_mp;
	xfs_buf_t		*bp;
	struct xfs_disk_dquot	*ddq, *recddq;
	xfs_failaddr_t		fa;
	int			error;
	xfs_dq_logformat_t	*dq_f;
	uint			type;


	/*
	 * Filesystems are required to send in quota flags at mount time.
	 */
	if (mp->m_qflags == 0)
		return 0;

	recddq = item->ri_buf[1].i_addr;
	if (recddq == NULL) {
		xfs_alert(log->l_mp, "NULL dquot in %s.", __func__);
		return -EFSCORRUPTED;
	}
	if (item->ri_buf[1].i_len < sizeof(struct xfs_disk_dquot)) {
		xfs_alert(log->l_mp, "dquot too small (%d) in %s.",
			item->ri_buf[1].i_len, __func__);
		return -EFSCORRUPTED;
	}

	/*
	 * This type of quotas was turned off, so ignore this record.
	 */
	type = recddq->d_flags & (XFS_DQ_USER | XFS_DQ_PROJ | XFS_DQ_GROUP);
	ASSERT(type);
	if (log->l_quotaoffs_flag & type)
		return 0;

	/*
	 * At this point we know that quota was _not_ turned off.
	 * Since the mount flags are not indicating to us otherwise, this
	 * must mean that quota is on, and the dquot needs to be replayed.
	 * Remember that we may not have fully recovered the superblock yet,
	 * so we can't do the usual trick of looking at the SB quota bits.
	 *
	 * The other possibility, of course, is that the quota subsystem was
	 * removed since the last mount - ENOSYS.
	 */
	dq_f = item->ri_buf[0].i_addr;
	ASSERT(dq_f);
	fa = xfs_dquot_verify(mp, recddq, dq_f->qlf_id, 0);
	if (fa) {
		xfs_alert(mp, "corrupt dquot ID 0x%x in log at %pS",
				dq_f->qlf_id, fa);
		return -EFSCORRUPTED;
	}
	ASSERT(dq_f->qlf_len == 1);

	/*
	 * At this point we are assuming that the dquots have been allocated
	 * and hence the buffer has valid dquots stamped in it. It should,
	 * therefore, pass verifier validation. If the dquot is bad, then the
	 * we'll return an error here, so we don't need to specifically check
	 * the dquot in the buffer after the verifier has run.
	 */
	error = xfs_trans_read_buf(mp, NULL, mp->m_ddev_targp, dq_f->qlf_blkno,
				   XFS_FSB_TO_BB(mp, dq_f->qlf_len), 0, &bp,
				   &xfs_dquot_buf_ops);
	if (error)
		return error;

	ASSERT(bp);
	ddq = xfs_buf_offset(bp, dq_f->qlf_boffset);

	/*
	 * If the dquot has an LSN in it, recover the dquot only if it's less
	 * than the lsn of the transaction we are replaying.
	 */
	if (xfs_sb_version_hascrc(&mp->m_sb)) {
		struct xfs_dqblk *dqb = (struct xfs_dqblk *)ddq;
		xfs_lsn_t	lsn = be64_to_cpu(dqb->dd_lsn);

		if (lsn && lsn != -1 && XFS_LSN_CMP(lsn, current_lsn) >= 0) {
			goto out_release;
		}
	}

	memcpy(ddq, recddq, item->ri_buf[1].i_len);
	if (xfs_sb_version_hascrc(&mp->m_sb)) {
		xfs_update_cksum((char *)ddq, sizeof(struct xfs_dqblk),
				 XFS_DQUOT_CRC_OFF);
	}

	ASSERT(dq_f->qlf_size == 2);
	ASSERT(bp->b_mount == mp);
	bp->b_iodone = xlog_recover_iodone;
	xfs_buf_delwri_queue(bp, buffer_list);

out_release:
	xfs_buf_relse(bp);
	return 0;
}

const struct xlog_recover_item_type xlog_dquot_item_type = {
	.reorder		= XLOG_REORDER_INODE_LIST,
	.ra_pass2_fn		= xlog_recover_dquot_ra_pass2,
	.commit_pass2_fn	= xlog_recover_dquot_pass2,
};

/*
 * Recover QUOTAOFF records. We simply make a note of it in the xlog
 * structure, so that we know not to do any dquot item or dquot buffer recovery,
 * of that type.
 */
STATIC int
xlog_recover_quotaoff_pass1(
	struct xlog			*log,
	struct xlog_recover_item	*item)
{
	xfs_qoff_logformat_t	*qoff_f = item->ri_buf[0].i_addr;
	ASSERT(qoff_f);

	/*
	 * The logitem format's flag tells us if this was user quotaoff,
	 * group/project quotaoff or both.
	 */
	if (qoff_f->qf_flags & XFS_UQUOTA_ACCT)
		log->l_quotaoffs_flag |= XFS_DQ_USER;
	if (qoff_f->qf_flags & XFS_PQUOTA_ACCT)
		log->l_quotaoffs_flag |= XFS_DQ_PROJ;
	if (qoff_f->qf_flags & XFS_GQUOTA_ACCT)
		log->l_quotaoffs_flag |= XFS_DQ_GROUP;

	return 0;
}

const struct xlog_recover_item_type xlog_quotaoff_item_type = {
	.reorder		= XLOG_REORDER_INODE_LIST,
	.commit_pass1_fn	= xlog_recover_quotaoff_pass1,
};
