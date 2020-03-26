// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_shared.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_trans_priv.h"
#include "xfs_swapext_item.h"
#include "xfs_swapext.h"
#include "xfs_log.h"
#include "xfs_bmap.h"
#include "xfs_icache.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans_space.h"
#include "xfs_error.h"
#include "xfs_log_priv.h"
#include "xfs_log_recover.h"

kmem_zone_t	*xfs_sxi_zone;
kmem_zone_t	*xfs_sxd_zone;

static inline struct xfs_sxi_log_item *SXI_ITEM(struct xfs_log_item *lip)
{
	return container_of(lip, struct xfs_sxi_log_item, sxi_item);
}

STATIC void
xfs_sxi_item_free(
	struct xfs_sxi_log_item	*ilip)
{
	kmem_cache_free(xfs_sxi_zone, ilip);
}

/*
 * Freeing the SXI requires that we remove it from the AIL if it has already
 * been placed there. However, the SXI may not yet have been placed in the AIL
 * when called by xfs_sxi_release() from SXD processing due to the ordering of
 * committed vs unpin operations in bulk insert operations. Hence the reference
 * count to ensure only the last caller frees the SXI.
 */
STATIC void
xfs_sxi_release(
	struct xfs_sxi_log_item	*ilip)
{
	ASSERT(atomic_read(&ilip->sxi_refcount) > 0);
	if (atomic_dec_and_test(&ilip->sxi_refcount)) {
		xfs_trans_ail_remove(&ilip->sxi_item, SHUTDOWN_LOG_IO_ERROR);
		xfs_sxi_item_free(ilip);
	}
}


STATIC void
xfs_sxi_item_size(
	struct xfs_log_item	*lip,
	int			*nvecs,
	int			*nbytes)
{
	*nvecs += 1;
	*nbytes += sizeof(struct xfs_sxi_log_format);
}

/*
 * This is called to fill in the vector of log iovecs for the
 * given sxi log item. We use only 1 iovec, and we point that
 * at the sxi_log_format structure embedded in the sxi item.
 * It is at this point that we assert that all of the extent
 * slots in the sxi item have been filled.
 */
STATIC void
xfs_sxi_item_format(
	struct xfs_log_item	*lip,
	struct xfs_log_vec	*lv)
{
	struct xfs_sxi_log_item	*ilip = SXI_ITEM(lip);
	struct xfs_log_iovec	*vecp = NULL;

	ilip->sxi_format.sxi_type = XFS_LI_SXI;
	ilip->sxi_format.sxi_size = 1;

	xlog_copy_iovec(lv, &vecp, XLOG_REG_TYPE_SXI_FORMAT, &ilip->sxi_format,
			sizeof(struct xfs_sxi_log_format));
}

/*
 * The unpin operation is the last place an SXI is manipulated in the log. It is
 * either inserted in the AIL or aborted in the event of a log I/O error. In
 * either case, the SXI transaction has been successfully committed to make it
 * this far. Therefore, we expect whoever committed the SXI to either construct
 * and commit the SXD or drop the SXD's reference in the event of error. Simply
 * drop the log's SXI reference now that the log is done with it.
 */
STATIC void
xfs_sxi_item_unpin(
	struct xfs_log_item	*lip,
	int			remove)
{
	struct xfs_sxi_log_item	*ilip = SXI_ITEM(lip);

	xfs_sxi_release(ilip);
}

/*
 * The SXI has been either committed or aborted if the transaction has been
 * cancelled. If the transaction was cancelled, an SXD isn't going to be
 * constructed and thus we free the SXI here directly.
 */
STATIC void
xfs_sxi_item_release(
	struct xfs_log_item	*lip)
{
	xfs_sxi_release(SXI_ITEM(lip));
}

static const struct xfs_item_ops xfs_sxi_item_ops = {
	.iop_size	= xfs_sxi_item_size,
	.iop_format	= xfs_sxi_item_format,
	.iop_unpin	= xfs_sxi_item_unpin,
	.iop_release	= xfs_sxi_item_release,
};

/*
 * Allocate and initialize an sxi item with the given number of extents.
 */
STATIC struct xfs_sxi_log_item *
xfs_sxi_init(
	struct xfs_mount		*mp)

{
	struct xfs_sxi_log_item		*ilip;

	ilip = kmem_zone_zalloc(xfs_sxi_zone, 0);

	xfs_log_item_init(mp, &ilip->sxi_item, XFS_LI_SXI, &xfs_sxi_item_ops);
	ilip->sxi_format.sxi_id = (uintptr_t)(void *)ilip;
	atomic_set(&ilip->sxi_refcount, 2);

	return ilip;
}

static inline struct xfs_sxd_log_item *SXD_ITEM(struct xfs_log_item *lip)
{
	return container_of(lip, struct xfs_sxd_log_item, sxd_item);
}

STATIC void
xfs_sxd_item_size(
	struct xfs_log_item	*lip,
	int			*nvecs,
	int			*nbytes)
{
	*nvecs += 1;
	*nbytes += sizeof(struct xfs_sxd_log_format);
}

/*
 * This is called to fill in the vector of log iovecs for the
 * given sxd log item. We use only 1 iovec, and we point that
 * at the sxd_log_format structure embedded in the sxd item.
 * It is at this point that we assert that all of the extent
 * slots in the sxd item have been filled.
 */
STATIC void
xfs_sxd_item_format(
	struct xfs_log_item	*lip,
	struct xfs_log_vec	*lv)
{
	struct xfs_sxd_log_item	*dlip = SXD_ITEM(lip);
	struct xfs_log_iovec	*vecp = NULL;

	dlip->sxd_format.sxd_type = XFS_LI_SXD;
	dlip->sxd_format.sxd_size = 1;

	xlog_copy_iovec(lv, &vecp, XLOG_REG_TYPE_SXD_FORMAT, &dlip->sxd_format,
			sizeof(struct xfs_sxd_log_format));
}

/*
 * The SXD is either committed or aborted if the transaction is cancelled. If
 * the transaction is cancelled, drop our reference to the SXI and free the
 * SXD.
 */
STATIC void
xfs_sxd_item_release(
	struct xfs_log_item	*lip)
{
	struct xfs_sxd_log_item	*dlip = SXD_ITEM(lip);

	xfs_sxi_release(dlip->sxd_intent_log_item);
	kmem_cache_free(xfs_sxd_zone, dlip);
}

static const struct xfs_item_ops xfs_sxd_item_ops = {
	.flags		= XFS_ITEM_RELEASE_WHEN_COMMITTED,
	.iop_size	= xfs_sxd_item_size,
	.iop_format	= xfs_sxd_item_format,
	.iop_release	= xfs_sxd_item_release,
};

static struct xfs_sxd_log_item *
xfs_trans_get_sxd(
	struct xfs_trans		*tp,
	struct xfs_sxi_log_item		*ilip)
{
	struct xfs_sxd_log_item		*dlip;

	dlip = kmem_zone_zalloc(xfs_sxd_zone, 0);
	xfs_log_item_init(tp->t_mountp, &dlip->sxd_item, XFS_LI_SXD,
			  &xfs_sxd_item_ops);
	dlip->sxd_intent_log_item = ilip;
	dlip->sxd_format.sxd_sxi_id = ilip->sxi_format.sxi_id;

	xfs_trans_add_item(tp, &dlip->sxd_item);
	return dlip;
}

/*
 * Finish an swapext update and log it to the SXD. Note that the
 * transaction is marked dirty regardless of whether the swapext update
 * succeeds or fails to support the SXI/SXD lifecycle rules.
 */
static int
xfs_trans_log_finish_swapext_update(
	struct xfs_trans		*tp,
	struct xfs_sxd_log_item		*dlip,
	struct xfs_swapext_intent	*sxi)
{
	int				error;

	error = xfs_swapext_finish_one(tp, sxi);

	/*
	 * Mark the transaction dirty, even on error. This ensures the
	 * transaction is aborted, which:
	 *
	 * 1.) releases the SXI and frees the SXD
	 * 2.) shuts down the filesystem
	 */
	tp->t_flags |= XFS_TRANS_DIRTY;
	set_bit(XFS_LI_DIRTY, &dlip->sxd_item.li_flags);

	return error;
}

/* Sort swapext intents by inode. */
static int
xfs_swapext_diff_items(
	void				*priv,
	struct list_head		*a,
	struct list_head		*b)
{
	struct xfs_swapext_intent	*sa;
	struct xfs_swapext_intent	*sb;

	sa = container_of(a, struct xfs_swapext_intent, si_list);
	sb = container_of(b, struct xfs_swapext_intent, si_list);
	return sa->si_ip1->i_ino - sb->si_ip2->i_ino;
}

/* Get an SXI. */
STATIC void *
xfs_swapext_create_intent(
	struct xfs_trans		*tp,
	unsigned int			count)
{
	struct xfs_sxi_log_item		*ilip;

	ASSERT(count == XFS_SXI_MAX_FAST_EXTENTS);
	ASSERT(tp != NULL);

	ilip = xfs_sxi_init(tp->t_mountp);
	ASSERT(ilip != NULL);

	/*
	 * Get a log_item_desc to point at the new item.
	 */
	xfs_trans_add_item(tp, &ilip->sxi_item);
	return ilip;
}

/* Log swapext updates in the intent item. */
STATIC void
xfs_swapext_log_item(
	struct xfs_trans		*tp,
	void				*intent,
	struct list_head		*item)
{
	struct xfs_sxi_log_item		*ilip = intent;
	struct xfs_swapext_intent	*sxi;
	struct xfs_swap_extent		*se;

	ASSERT(!test_bit(XFS_LI_DIRTY, &ilip->sxi_item.li_flags));

	sxi = container_of(item, struct xfs_swapext_intent, si_list);

	tp->t_flags |= XFS_TRANS_DIRTY;
	set_bit(XFS_LI_DIRTY, &ilip->sxi_item.li_flags);

	se = &ilip->sxi_format.sxi_extent;
	se->se_inode1 = sxi->si_ip1->i_ino;
	se->se_inode2 = sxi->si_ip2->i_ino;
	se->se_startoff1 = sxi->si_startoff1;
	se->se_startoff2 = sxi->si_startoff2;
	se->se_blockcount = sxi->si_blockcount;
	se->se_isize1 = sxi->si_isize1;
	se->se_isize2 = sxi->si_isize2;
	se->se_flags = sxi->si_flags;
}

/* Get an SXD so we can process all the deferred swapext updates. */
STATIC void *
xfs_swapext_create_done(
	struct xfs_trans		*tp,
	void				*intent,
	unsigned int			count)
{
	return xfs_trans_get_sxd(tp, intent);
}

/* Process a deferred swapext update. */
STATIC int
xfs_swapext_finish_item(
	struct xfs_trans		*tp,
	struct list_head		*item,
	void				*done_item,
	void				**state)
{
	struct xfs_swapext_intent	*sxi;
	int				error;

	sxi = container_of(item, struct xfs_swapext_intent, si_list);

	/*
	 * Swap one more extent between the two files.  If there's still more
	 * work to do, we want to requeue ourselves after all other pending
	 * deferred operations have finished.  This includes all of the dfops
	 * that we queued directly as well as any new ones created in the
	 * process of finishing the others.  Doing so prevents us from queuing
	 * a large number of SXI log items in kernel memory, which in turn
	 * prevents us from pinning the tail of the log (while logging those
	 * new SXI items) until the first SXI items can be processed.
	 */
	error = xfs_trans_log_finish_swapext_update(tp, done_item, sxi);
	if (!error && xfs_swapext_has_more_work(sxi))
		return -EMULTIHOP;

	kmem_free(sxi);
	return error;
}

/* Abort all pending SXIs. */
STATIC void
xfs_swapext_abort_intent(
	void				*intent)
{
	xfs_sxi_release(intent);
}

/* Cancel a deferred swapext update. */
STATIC void
xfs_swapext_cancel_item(
	struct list_head		*item)
{
	struct xfs_swapext_intent	*sxi;

	sxi = container_of(item, struct xfs_swapext_intent, si_list);
	kmem_free(sxi);
}

/* Prepare a deferred swapext item for freezing by detaching the inodes. */
STATIC int
xfs_swapext_freeze_item(
	struct xfs_defer_freezer	*freezer,
	struct list_head		*item)
{
	struct xfs_swapext_intent	*sxi;
	struct xfs_inode		*ip;
	int				error;

	sxi = container_of(item, struct xfs_swapext_intent, si_list);

	ip = sxi->si_ip1;
	error = xfs_defer_freezer_ijoin(freezer, ip);
	if (error)
		return error;
	sxi->si_ino1 = ip->i_ino;

	ip = sxi->si_ip2;
	error = xfs_defer_freezer_ijoin(freezer, ip);
	if (error)
		return error;
	sxi->si_ino2 = ip->i_ino;

	return 0;
}

/* Thaw a deferred swapext item by reattaching the inodes. */
STATIC int
xfs_swapext_thaw_item(
	struct xfs_defer_freezer	*freezer,
	struct list_head		*item)
{
	struct xfs_swapext_intent	*sxi;
	struct xfs_inode		*ip;

	sxi = container_of(item, struct xfs_swapext_intent, si_list);

	ip = xfs_defer_freezer_igrab(freezer, sxi->si_ino1);
	if (!ip)
		return -EFSCORRUPTED;
	sxi->si_ip1 = ip;

	ip = xfs_defer_freezer_igrab(freezer, sxi->si_ino2);
	if (!ip)
		return -EFSCORRUPTED;
	sxi->si_ip2 = ip;

	return 0;
}

const struct xfs_defer_op_type xfs_swapext_defer_type = {
	.max_items	= XFS_SXI_MAX_FAST_EXTENTS,
	.diff_items	= xfs_swapext_diff_items,
	.create_intent	= xfs_swapext_create_intent,
	.abort_intent	= xfs_swapext_abort_intent,
	.log_item	= xfs_swapext_log_item,
	.create_done	= xfs_swapext_create_done,
	.finish_item	= xfs_swapext_finish_item,
	.cancel_item	= xfs_swapext_cancel_item,
	.freeze_item	= xfs_swapext_freeze_item,
	.thaw_item	= xfs_swapext_thaw_item,
};

/*
 * Process a swapext update intent item that was recovered from the log.
 * We need to update some inode's bmbt.
 */
STATIC int
xfs_sxi_recover(
	struct xfs_mount		*mp,
	struct xfs_defer_freezer	**dffp,
	struct xfs_sxi_log_item		*ilip)
{
	struct xfs_swapext_intent	sxi;
	struct xfs_swap_extent		*se;
	struct xfs_sxd_log_item		*dlip;
	struct xfs_trans		*tp;
	int				error = 0;

	ASSERT(!test_bit(XFS_SXI_RECOVERED, &ilip->sxi_flags));

	/*
	 * First check the validity of the extent described by the
	 * SXI.  If anything is bad, then toss the SXI.
	 */
	se = &ilip->sxi_format.sxi_extent;
	if (se->se_blockcount == 0 ||
	    ilip->sxi_format.__pad != 0 ||
	    !xfs_verify_ino(mp, se->se_inode1) ||
	    !xfs_verify_ino(mp, se->se_inode2) ||
	    (se->se_flags & ~XFS_SWAP_EXTENT_FLAGS) ||
	    ((se->se_flags & XFS_SWAP_EXTENT_SET_SIZES) &&
	     (se->se_isize1 < 0 || se->se_isize2 < 0))) {
		/*
		 * This will pull the SXI from the AIL and
		 * free the memory associated with it.
		 */
		set_bit(XFS_SXI_RECOVERED, &ilip->sxi_flags);
		xfs_sxi_release(ilip);
		return -EFSCORRUPTED;
	}

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate,
			XFS_EXTENTADD_SPACE_RES(mp, XFS_DATA_FORK), 0, 0, &tp);
	if (error)
		return error;

	dlip = xfs_trans_get_sxd(tp, ilip);
	memset(&sxi, 0, sizeof(sxi));
	INIT_LIST_HEAD(&sxi.si_list);

	/* Grab both inodes and lock them. */
	error = xfs_iget(mp, tp, se->se_inode1, 0, 0, &sxi.si_ip1);
	if (error)
		goto out_fail;
	error = xfs_iget(mp, tp, se->se_inode2, 0, 0, &sxi.si_ip2);
	if (error)
		goto out_fail;

	xfs_lock_two_inodes(sxi.si_ip1, XFS_ILOCK_EXCL,
			    sxi.si_ip2, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, sxi.si_ip1, 0);
	xfs_trans_ijoin(tp, sxi.si_ip2, 0);

	/*
	 * Set IRECOVERY to prevent trimming of post-eof extents and freeing of
	 * unlinked inodes until we're totally done processing files.
	 */
	if (VFS_I(sxi.si_ip1)->i_nlink == 0)
		xfs_iflags_set(sxi.si_ip1, XFS_IRECOVERY);
	if (VFS_I(sxi.si_ip2)->i_nlink == 0)
		xfs_iflags_set(sxi.si_ip2, XFS_IRECOVERY);

	/*
	 * Construct the rest of our in-core swapext intent state so that we
	 * can call the deferred operation functions to continue the work.
	 */
	sxi.si_flags = se->se_flags;
	sxi.si_startoff1 = se->se_startoff1;
	sxi.si_startoff2 = se->se_startoff2;
	sxi.si_blockcount = se->se_blockcount;
	sxi.si_isize1 = se->se_isize1;
	sxi.si_isize2 = se->se_isize2;
	error = xfs_trans_log_finish_swapext_update(tp, dlip, &sxi);
	if (error)
		goto out_fail;

	/*
	 * If there's more extent swapping to be done, we have to schedule that
	 * as a separate deferred operation to be run after we've finished
	 * replaying all of the intents we recovered from the log.
	 */
	if (xfs_swapext_has_more_work(&sxi))
		xfs_swapext_reschedule(tp, &sxi);

	set_bit(XFS_SXI_RECOVERED, &ilip->sxi_flags);
	error = xlog_recover_trans_commit(tp, dffp);
	goto out_rele;

out_fail:
	xfs_trans_cancel(tp);
out_rele:
	if (sxi.si_ip2) {
		xfs_iunlock(sxi.si_ip2, XFS_ILOCK_EXCL);
		xfs_irele(sxi.si_ip2);
	}
	if (sxi.si_ip1) {
		xfs_iunlock(sxi.si_ip1, XFS_ILOCK_EXCL);
		xfs_irele(sxi.si_ip1);
	}
	return error;

}

/*
 * Copy an SXI format buffer from the given buf, and into the destination
 * SXI format structure.  The SXI/SXD items were designed not to need any
 * special alignment handling.
 */
static int
xfs_sxi_copy_format(
	struct xfs_log_iovec		*buf,
	struct xfs_sxi_log_format	*dst_sxi_fmt)
{
	struct xfs_sxi_log_format	*src_sxi_fmt;
	size_t				len;

	src_sxi_fmt = buf->i_addr;
	len = sizeof(struct xfs_sxi_log_format);

	if (buf->i_len == len) {
		memcpy(dst_sxi_fmt, src_sxi_fmt, len);
		return 0;
	}
	XFS_ERROR_REPORT(__func__, XFS_ERRLEVEL_LOW, NULL);
	return -EFSCORRUPTED;
}

/*
 * This routine is called to create an in-core extent swapext update
 * item from the sxi format structure which was logged on disk.
 * It allocates an in-core sxi, copies the extents from the format
 * structure into it, and adds the sxi to the AIL with the given
 * LSN.
 */
STATIC int
xlog_recover_sxi(
	struct xlog			*log,
	struct xlog_recover_item	*item,
	xfs_lsn_t			lsn)
{
	int				error;
	struct xfs_mount		*mp = log->l_mp;
	struct xfs_sxi_log_item		*ilip;
	struct xfs_sxi_log_format	*sxi_formatp;

	sxi_formatp = item->ri_buf[0].i_addr;

	if (sxi_formatp->__pad != 0) {
		XFS_ERROR_REPORT(__func__, XFS_ERRLEVEL_LOW, log->l_mp);
		return -EFSCORRUPTED;
	}
	ilip = xfs_sxi_init(mp);
	error = xfs_sxi_copy_format(&item->ri_buf[0], &ilip->sxi_format);
	if (error) {
		xfs_sxi_item_free(ilip);
		return error;
	}
	xlog_recover_insert_ail(log, &ilip->sxi_item, lsn);
	xfs_sxi_release(ilip);
	return 0;
}

STATIC bool
xlog_release_sxi(
	struct xlog		*log,
	struct xfs_log_item	*lip,
	uint64_t		intent_id)
{
	struct xfs_sxi_log_item	*ilip = SXI_ITEM(lip);
	struct xfs_ail		*ailp = log->l_ailp;

	if (ilip->sxi_format.sxi_id == intent_id) {
		/*
		 * Drop the SXD reference to the SXI. This
		 * removes the SXI from the AIL and frees it.
		 */
		spin_unlock(&ailp->ail_lock);
		xfs_sxi_release(ilip);
		spin_lock(&ailp->ail_lock);
		return true;
	}

	return false;
}

/*
 * This routine is called when an SXD format structure is found in a committed
 * transaction in the log. Its purpose is to cancel the corresponding SXI if it
 * was still in the log. To do this it searches the AIL for the SXI with an id
 * equal to that in the SXD format structure. If we find it we drop the SXD
 * reference, which removes the SXI from the AIL and frees it.
 */
STATIC int
xlog_recover_sxd(
	struct xlog			*log,
	struct xlog_recover_item	*item)
{
	struct xfs_sxd_log_format	*sxd_formatp;

	sxd_formatp = item->ri_buf[0].i_addr;
	if (item->ri_buf[0].i_len != sizeof(struct xfs_sxd_log_format)) {
		XFS_ERROR_REPORT(__func__, XFS_ERRLEVEL_LOW, log->l_mp);
		return -EFSCORRUPTED;
	}

	xlog_recover_release_intent(log, XFS_LI_SXI, sxd_formatp->sxd_sxi_id,
			 xlog_release_sxi);
	return 0;
}

/* Recover the SXI if necessary. */
STATIC int
xlog_recover_process_sxi(
	struct xlog			*log,
	struct xfs_defer_freezer	**dffp,
	struct xfs_log_item		*lip)
{
	struct xfs_ail			*ailp = log->l_ailp;
	struct xfs_sxi_log_item		*ilip = SXI_ITEM(lip);
	int				error;

	/*
	 * Skip SXIs that we've already processed.
	 */
	if (test_bit(XFS_SXI_RECOVERED, &ilip->sxi_flags))
		return 0;

	spin_unlock(&ailp->ail_lock);
	error = xfs_sxi_recover(log->l_mp, dffp, ilip);
	spin_lock(&ailp->ail_lock);

	return error;
}

/* Release the SXI since we're cancelling everything. */
STATIC void
xlog_recover_cancel_sxi(
	struct xfs_log_item		*lip)
{
	xfs_sxi_release(SXI_ITEM(lip));
}

const struct xlog_recover_intent_type xlog_recover_swapext_type = {
	.recover_intent		= xlog_recover_sxi,
	.recover_done		= xlog_recover_sxd,
	.process_intent		= xlog_recover_process_sxi,
	.cancel_intent		= xlog_recover_cancel_sxi,
};
