// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_mount.h"
#include "xfs_trans.h"
#include "xfs_buf_item.h"
#include "xfs_trans_priv.h"
#include "xfs_trace.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_log_recover.h"
#include "xfs_error.h"
#include "xfs_inode.h"
#include "xfs_dir2.h"
#include "xfs_quota.h"

kmem_zone_t	*xfs_buf_item_zone;

static inline struct xfs_buf_log_item *BUF_ITEM(struct xfs_log_item *lip)
{
	return container_of(lip, struct xfs_buf_log_item, bli_item);
}

STATIC void	xfs_buf_do_callbacks(struct xfs_buf *bp);

/* Is this log iovec plausibly large enough to contain the buffer log format? */
STATIC bool
xfs_buf_log_check_iovec(
	struct xfs_log_iovec		*iovec)
{
	struct xfs_buf_log_format	*blfp = iovec->i_addr;
	char				*bmp_end;
	char				*item_end;

	if (offsetof(struct xfs_buf_log_format, blf_data_map) > iovec->i_len)
		return false;

	item_end = (char *)iovec->i_addr + iovec->i_len;
	bmp_end = (char *)&blfp->blf_data_map[blfp->blf_map_size];
	return bmp_end <= item_end;
}

static inline int
xfs_buf_log_format_size(
	struct xfs_buf_log_format *blfp)
{
	return offsetof(struct xfs_buf_log_format, blf_data_map) +
			(blfp->blf_map_size * sizeof(blfp->blf_data_map[0]));
}

/*
 * This returns the number of log iovecs needed to log the
 * given buf log item.
 *
 * It calculates this as 1 iovec for the buf log format structure
 * and 1 for each stretch of non-contiguous chunks to be logged.
 * Contiguous chunks are logged in a single iovec.
 *
 * If the XFS_BLI_STALE flag has been set, then log nothing.
 */
STATIC void
xfs_buf_item_size_segment(
	struct xfs_buf_log_item		*bip,
	struct xfs_buf_log_format	*blfp,
	int				*nvecs,
	int				*nbytes)
{
	struct xfs_buf			*bp = bip->bli_buf;
	int				next_bit;
	int				last_bit;

	last_bit = xfs_next_bit(blfp->blf_data_map, blfp->blf_map_size, 0);
	if (last_bit == -1)
		return;

	/*
	 * initial count for a dirty buffer is 2 vectors - the format structure
	 * and the first dirty region.
	 */
	*nvecs += 2;
	*nbytes += xfs_buf_log_format_size(blfp) + XFS_BLF_CHUNK;

	while (last_bit != -1) {
		/*
		 * This takes the bit number to start looking from and
		 * returns the next set bit from there.  It returns -1
		 * if there are no more bits set or the start bit is
		 * beyond the end of the bitmap.
		 */
		next_bit = xfs_next_bit(blfp->blf_data_map, blfp->blf_map_size,
					last_bit + 1);
		/*
		 * If we run out of bits, leave the loop,
		 * else if we find a new set of bits bump the number of vecs,
		 * else keep scanning the current set of bits.
		 */
		if (next_bit == -1) {
			break;
		} else if (next_bit != last_bit + 1) {
			last_bit = next_bit;
			(*nvecs)++;
		} else if (xfs_buf_offset(bp, next_bit * XFS_BLF_CHUNK) !=
			   (xfs_buf_offset(bp, last_bit * XFS_BLF_CHUNK) +
			    XFS_BLF_CHUNK)) {
			last_bit = next_bit;
			(*nvecs)++;
		} else {
			last_bit++;
		}
		*nbytes += XFS_BLF_CHUNK;
	}
}

/*
 * This returns the number of log iovecs needed to log the given buf log item.
 *
 * It calculates this as 1 iovec for the buf log format structure and 1 for each
 * stretch of non-contiguous chunks to be logged.  Contiguous chunks are logged
 * in a single iovec.
 *
 * Discontiguous buffers need a format structure per region that that is being
 * logged. This makes the changes in the buffer appear to log recovery as though
 * they came from separate buffers, just like would occur if multiple buffers
 * were used instead of a single discontiguous buffer. This enables
 * discontiguous buffers to be in-memory constructs, completely transparent to
 * what ends up on disk.
 *
 * If the XFS_BLI_STALE flag has been set, then log nothing but the buf log
 * format structures.
 */
STATIC void
xfs_buf_item_size(
	struct xfs_log_item	*lip,
	int			*nvecs,
	int			*nbytes)
{
	struct xfs_buf_log_item	*bip = BUF_ITEM(lip);
	int			i;

	ASSERT(atomic_read(&bip->bli_refcount) > 0);
	if (bip->bli_flags & XFS_BLI_STALE) {
		/*
		 * The buffer is stale, so all we need to log
		 * is the buf log format structure with the
		 * cancel flag in it.
		 */
		trace_xfs_buf_item_size_stale(bip);
		ASSERT(bip->__bli_format.blf_flags & XFS_BLF_CANCEL);
		*nvecs += bip->bli_format_count;
		for (i = 0; i < bip->bli_format_count; i++) {
			*nbytes += xfs_buf_log_format_size(&bip->bli_formats[i]);
		}
		return;
	}

	ASSERT(bip->bli_flags & XFS_BLI_LOGGED);

	if (bip->bli_flags & XFS_BLI_ORDERED) {
		/*
		 * The buffer has been logged just to order it.
		 * It is not being included in the transaction
		 * commit, so no vectors are used at all.
		 */
		trace_xfs_buf_item_size_ordered(bip);
		*nvecs = XFS_LOG_VEC_ORDERED;
		return;
	}

	/*
	 * the vector count is based on the number of buffer vectors we have
	 * dirty bits in. This will only be greater than one when we have a
	 * compound buffer with more than one segment dirty. Hence for compound
	 * buffers we need to track which segment the dirty bits correspond to,
	 * and when we move from one segment to the next increment the vector
	 * count for the extra buf log format structure that will need to be
	 * written.
	 */
	for (i = 0; i < bip->bli_format_count; i++) {
		xfs_buf_item_size_segment(bip, &bip->bli_formats[i],
					  nvecs, nbytes);
	}
	trace_xfs_buf_item_size(bip);
}

static inline void
xfs_buf_item_copy_iovec(
	struct xfs_log_vec	*lv,
	struct xfs_log_iovec	**vecp,
	struct xfs_buf		*bp,
	uint			offset,
	int			first_bit,
	uint			nbits)
{
	offset += first_bit * XFS_BLF_CHUNK;
	xlog_copy_iovec(lv, vecp, XLOG_REG_TYPE_BCHUNK,
			xfs_buf_offset(bp, offset),
			nbits * XFS_BLF_CHUNK);
}

static inline bool
xfs_buf_item_straddle(
	struct xfs_buf		*bp,
	uint			offset,
	int			next_bit,
	int			last_bit)
{
	return xfs_buf_offset(bp, offset + (next_bit << XFS_BLF_SHIFT)) !=
		(xfs_buf_offset(bp, offset + (last_bit << XFS_BLF_SHIFT)) +
		 XFS_BLF_CHUNK);
}

static void
xfs_buf_item_format_segment(
	struct xfs_buf_log_item	*bip,
	struct xfs_log_vec	*lv,
	struct xfs_log_iovec	**vecp,
	uint			offset,
	struct xfs_buf_log_format *blfp)
{
	struct xfs_buf		*bp = bip->bli_buf;
	uint			base_size;
	int			first_bit;
	int			last_bit;
	int			next_bit;
	uint			nbits;

	/* copy the flags across from the base format item */
	blfp->blf_flags = bip->__bli_format.blf_flags;

	/*
	 * Base size is the actual size of the ondisk structure - it reflects
	 * the actual size of the dirty bitmap rather than the size of the in
	 * memory structure.
	 */
	base_size = xfs_buf_log_format_size(blfp);

	first_bit = xfs_next_bit(blfp->blf_data_map, blfp->blf_map_size, 0);
	if (!(bip->bli_flags & XFS_BLI_STALE) && first_bit == -1) {
		/*
		 * If the map is not be dirty in the transaction, mark
		 * the size as zero and do not advance the vector pointer.
		 */
		return;
	}

	blfp = xlog_copy_iovec(lv, vecp, XLOG_REG_TYPE_BFORMAT, blfp, base_size);
	blfp->blf_size = 1;

	if (bip->bli_flags & XFS_BLI_STALE) {
		/*
		 * The buffer is stale, so all we need to log
		 * is the buf log format structure with the
		 * cancel flag in it.
		 */
		trace_xfs_buf_item_format_stale(bip);
		ASSERT(blfp->blf_flags & XFS_BLF_CANCEL);
		return;
	}


	/*
	 * Fill in an iovec for each set of contiguous chunks.
	 */
	last_bit = first_bit;
	nbits = 1;
	for (;;) {
		/*
		 * This takes the bit number to start looking from and
		 * returns the next set bit from there.  It returns -1
		 * if there are no more bits set or the start bit is
		 * beyond the end of the bitmap.
		 */
		next_bit = xfs_next_bit(blfp->blf_data_map, blfp->blf_map_size,
					(uint)last_bit + 1);
		/*
		 * If we run out of bits fill in the last iovec and get out of
		 * the loop.  Else if we start a new set of bits then fill in
		 * the iovec for the series we were looking at and start
		 * counting the bits in the new one.  Else we're still in the
		 * same set of bits so just keep counting and scanning.
		 */
		if (next_bit == -1) {
			xfs_buf_item_copy_iovec(lv, vecp, bp, offset,
						first_bit, nbits);
			blfp->blf_size++;
			break;
		} else if (next_bit != last_bit + 1 ||
		           xfs_buf_item_straddle(bp, offset, next_bit, last_bit)) {
			xfs_buf_item_copy_iovec(lv, vecp, bp, offset,
						first_bit, nbits);
			blfp->blf_size++;
			first_bit = next_bit;
			last_bit = next_bit;
			nbits = 1;
		} else {
			last_bit++;
			nbits++;
		}
	}
}

/*
 * This is called to fill in the vector of log iovecs for the
 * given log buf item.  It fills the first entry with a buf log
 * format structure, and the rest point to contiguous chunks
 * within the buffer.
 */
STATIC void
xfs_buf_item_format(
	struct xfs_log_item	*lip,
	struct xfs_log_vec	*lv)
{
	struct xfs_buf_log_item	*bip = BUF_ITEM(lip);
	struct xfs_buf		*bp = bip->bli_buf;
	struct xfs_log_iovec	*vecp = NULL;
	uint			offset = 0;
	int			i;

	ASSERT(atomic_read(&bip->bli_refcount) > 0);
	ASSERT((bip->bli_flags & XFS_BLI_LOGGED) ||
	       (bip->bli_flags & XFS_BLI_STALE));
	ASSERT((bip->bli_flags & XFS_BLI_STALE) ||
	       (xfs_blft_from_flags(&bip->__bli_format) > XFS_BLFT_UNKNOWN_BUF
	        && xfs_blft_from_flags(&bip->__bli_format) < XFS_BLFT_MAX_BUF));
	ASSERT(!(bip->bli_flags & XFS_BLI_ORDERED) ||
	       (bip->bli_flags & XFS_BLI_STALE));


	/*
	 * If it is an inode buffer, transfer the in-memory state to the
	 * format flags and clear the in-memory state.
	 *
	 * For buffer based inode allocation, we do not transfer
	 * this state if the inode buffer allocation has not yet been committed
	 * to the log as setting the XFS_BLI_INODE_BUF flag will prevent
	 * correct replay of the inode allocation.
	 *
	 * For icreate item based inode allocation, the buffers aren't written
	 * to the journal during allocation, and hence we should always tag the
	 * buffer as an inode buffer so that the correct unlinked list replay
	 * occurs during recovery.
	 */
	if (bip->bli_flags & XFS_BLI_INODE_BUF) {
		if (xfs_sb_version_has_v3inode(&lip->li_mountp->m_sb) ||
		    !((bip->bli_flags & XFS_BLI_INODE_ALLOC_BUF) &&
		      xfs_log_item_in_current_chkpt(lip)))
			bip->__bli_format.blf_flags |= XFS_BLF_INODE_BUF;
		bip->bli_flags &= ~XFS_BLI_INODE_BUF;
	}

	for (i = 0; i < bip->bli_format_count; i++) {
		xfs_buf_item_format_segment(bip, lv, &vecp, offset,
					    &bip->bli_formats[i]);
		offset += BBTOB(bp->b_maps[i].bm_len);
	}

	/*
	 * Check to make sure everything is consistent.
	 */
	trace_xfs_buf_item_format(bip);
}

/*
 * This is called to pin the buffer associated with the buf log item in memory
 * so it cannot be written out.
 *
 * We also always take a reference to the buffer log item here so that the bli
 * is held while the item is pinned in memory. This means that we can
 * unconditionally drop the reference count a transaction holds when the
 * transaction is completed.
 */
STATIC void
xfs_buf_item_pin(
	struct xfs_log_item	*lip)
{
	struct xfs_buf_log_item	*bip = BUF_ITEM(lip);

	ASSERT(atomic_read(&bip->bli_refcount) > 0);
	ASSERT((bip->bli_flags & XFS_BLI_LOGGED) ||
	       (bip->bli_flags & XFS_BLI_ORDERED) ||
	       (bip->bli_flags & XFS_BLI_STALE));

	trace_xfs_buf_item_pin(bip);

	atomic_inc(&bip->bli_refcount);
	atomic_inc(&bip->bli_buf->b_pin_count);
}

/*
 * This is called to unpin the buffer associated with the buf log
 * item which was previously pinned with a call to xfs_buf_item_pin().
 *
 * Also drop the reference to the buf item for the current transaction.
 * If the XFS_BLI_STALE flag is set and we are the last reference,
 * then free up the buf log item and unlock the buffer.
 *
 * If the remove flag is set we are called from uncommit in the
 * forced-shutdown path.  If that is true and the reference count on
 * the log item is going to drop to zero we need to free the item's
 * descriptor in the transaction.
 */
STATIC void
xfs_buf_item_unpin(
	struct xfs_log_item	*lip,
	int			remove)
{
	struct xfs_buf_log_item	*bip = BUF_ITEM(lip);
	xfs_buf_t		*bp = bip->bli_buf;
	struct xfs_ail		*ailp = lip->li_ailp;
	int			stale = bip->bli_flags & XFS_BLI_STALE;
	int			freed;

	ASSERT(bp->b_log_item == bip);
	ASSERT(atomic_read(&bip->bli_refcount) > 0);

	trace_xfs_buf_item_unpin(bip);

	freed = atomic_dec_and_test(&bip->bli_refcount);

	if (atomic_dec_and_test(&bp->b_pin_count))
		wake_up_all(&bp->b_waiters);

	if (freed && stale) {
		ASSERT(bip->bli_flags & XFS_BLI_STALE);
		ASSERT(xfs_buf_islocked(bp));
		ASSERT(bp->b_flags & XBF_STALE);
		ASSERT(bip->__bli_format.blf_flags & XFS_BLF_CANCEL);

		trace_xfs_buf_item_unpin_stale(bip);

		if (remove) {
			/*
			 * If we are in a transaction context, we have to
			 * remove the log item from the transaction as we are
			 * about to release our reference to the buffer.  If we
			 * don't, the unlock that occurs later in
			 * xfs_trans_uncommit() will try to reference the
			 * buffer which we no longer have a hold on.
			 */
			if (!list_empty(&lip->li_trans))
				xfs_trans_del_item(lip);

			/*
			 * Since the transaction no longer refers to the buffer,
			 * the buffer should no longer refer to the transaction.
			 */
			bp->b_transp = NULL;
		}

		/*
		 * If we get called here because of an IO error, we may
		 * or may not have the item on the AIL. xfs_trans_ail_delete()
		 * will take care of that situation.
		 * xfs_trans_ail_delete() drops the AIL lock.
		 */
		if (bip->bli_flags & XFS_BLI_STALE_INODE) {
			xfs_buf_do_callbacks(bp);
			bp->b_log_item = NULL;
			list_del_init(&bp->b_li_list);
			bp->b_iodone = NULL;
		} else {
			spin_lock(&ailp->ail_lock);
			xfs_trans_ail_delete(ailp, lip, SHUTDOWN_LOG_IO_ERROR);
			xfs_buf_item_relse(bp);
			ASSERT(bp->b_log_item == NULL);
		}
		xfs_buf_relse(bp);
	} else if (freed && remove) {
		/*
		 * There are currently two references to the buffer - the active
		 * LRU reference and the buf log item. What we are about to do
		 * here - simulate a failed IO completion - requires 3
		 * references.
		 *
		 * The LRU reference is removed by the xfs_buf_stale() call. The
		 * buf item reference is removed by the xfs_buf_iodone()
		 * callback that is run by xfs_buf_do_callbacks() during ioend
		 * processing (via the bp->b_iodone callback), and then finally
		 * the ioend processing will drop the IO reference if the buffer
		 * is marked XBF_ASYNC.
		 *
		 * Hence we need to take an additional reference here so that IO
		 * completion processing doesn't free the buffer prematurely.
		 */
		xfs_buf_lock(bp);
		xfs_buf_hold(bp);
		bp->b_flags |= XBF_ASYNC;
		xfs_buf_ioerror(bp, -EIO);
		bp->b_flags &= ~XBF_DONE;
		xfs_buf_stale(bp);
		xfs_buf_ioend(bp);
	}
}

/*
 * Buffer IO error rate limiting. Limit it to no more than 10 messages per 30
 * seconds so as to not spam logs too much on repeated detection of the same
 * buffer being bad..
 */

static DEFINE_RATELIMIT_STATE(xfs_buf_write_fail_rl_state, 30 * HZ, 10);

STATIC uint
xfs_buf_item_push(
	struct xfs_log_item	*lip,
	struct list_head	*buffer_list)
{
	struct xfs_buf_log_item	*bip = BUF_ITEM(lip);
	struct xfs_buf		*bp = bip->bli_buf;
	uint			rval = XFS_ITEM_SUCCESS;

	if (xfs_buf_ispinned(bp))
		return XFS_ITEM_PINNED;
	if (!xfs_buf_trylock(bp)) {
		/*
		 * If we have just raced with a buffer being pinned and it has
		 * been marked stale, we could end up stalling until someone else
		 * issues a log force to unpin the stale buffer. Check for the
		 * race condition here so xfsaild recognizes the buffer is pinned
		 * and queues a log force to move it along.
		 */
		if (xfs_buf_ispinned(bp))
			return XFS_ITEM_PINNED;
		return XFS_ITEM_LOCKED;
	}

	ASSERT(!(bip->bli_flags & XFS_BLI_STALE));

	trace_xfs_buf_item_push(bip);

	/* has a previous flush failed due to IO errors? */
	if ((bp->b_flags & XBF_WRITE_FAIL) &&
	    ___ratelimit(&xfs_buf_write_fail_rl_state, "XFS: Failing async write")) {
		xfs_warn(bp->b_mount,
"Failing async write on buffer block 0x%llx. Retrying async write.",
			 (long long)bp->b_bn);
	}

	if (!xfs_buf_delwri_queue(bp, buffer_list))
		rval = XFS_ITEM_FLUSHING;
	xfs_buf_unlock(bp);
	return rval;
}

/*
 * Drop the buffer log item refcount and take appropriate action. This helper
 * determines whether the bli must be freed or not, since a decrement to zero
 * does not necessarily mean the bli is unused.
 *
 * Return true if the bli is freed, false otherwise.
 */
bool
xfs_buf_item_put(
	struct xfs_buf_log_item	*bip)
{
	struct xfs_log_item	*lip = &bip->bli_item;
	bool			aborted;
	bool			dirty;

	/* drop the bli ref and return if it wasn't the last one */
	if (!atomic_dec_and_test(&bip->bli_refcount))
		return false;

	/*
	 * We dropped the last ref and must free the item if clean or aborted.
	 * If the bli is dirty and non-aborted, the buffer was clean in the
	 * transaction but still awaiting writeback from previous changes. In
	 * that case, the bli is freed on buffer writeback completion.
	 */
	aborted = test_bit(XFS_LI_ABORTED, &lip->li_flags) ||
		  XFS_FORCED_SHUTDOWN(lip->li_mountp);
	dirty = bip->bli_flags & XFS_BLI_DIRTY;
	if (dirty && !aborted)
		return false;

	/*
	 * The bli is aborted or clean. An aborted item may be in the AIL
	 * regardless of dirty state.  For example, consider an aborted
	 * transaction that invalidated a dirty bli and cleared the dirty
	 * state.
	 */
	if (aborted)
		xfs_trans_ail_remove(lip, SHUTDOWN_LOG_IO_ERROR);
	xfs_buf_item_relse(bip->bli_buf);
	return true;
}

/*
 * Release the buffer associated with the buf log item.  If there is no dirty
 * logged data associated with the buffer recorded in the buf log item, then
 * free the buf log item and remove the reference to it in the buffer.
 *
 * This call ignores the recursion count.  It is only called when the buffer
 * should REALLY be unlocked, regardless of the recursion count.
 *
 * We unconditionally drop the transaction's reference to the log item. If the
 * item was logged, then another reference was taken when it was pinned, so we
 * can safely drop the transaction reference now.  This also allows us to avoid
 * potential races with the unpin code freeing the bli by not referencing the
 * bli after we've dropped the reference count.
 *
 * If the XFS_BLI_HOLD flag is set in the buf log item, then free the log item
 * if necessary but do not unlock the buffer.  This is for support of
 * xfs_trans_bhold(). Make sure the XFS_BLI_HOLD field is cleared if we don't
 * free the item.
 */
STATIC void
xfs_buf_item_release(
	struct xfs_log_item	*lip)
{
	struct xfs_buf_log_item	*bip = BUF_ITEM(lip);
	struct xfs_buf		*bp = bip->bli_buf;
	bool			released;
	bool			hold = bip->bli_flags & XFS_BLI_HOLD;
	bool			stale = bip->bli_flags & XFS_BLI_STALE;
#if defined(DEBUG) || defined(XFS_WARN)
	bool			ordered = bip->bli_flags & XFS_BLI_ORDERED;
	bool			dirty = bip->bli_flags & XFS_BLI_DIRTY;
	bool			aborted = test_bit(XFS_LI_ABORTED,
						   &lip->li_flags);
#endif

	trace_xfs_buf_item_release(bip);

	/*
	 * The bli dirty state should match whether the blf has logged segments
	 * except for ordered buffers, where only the bli should be dirty.
	 */
	ASSERT((!ordered && dirty == xfs_buf_item_dirty_format(bip)) ||
	       (ordered && dirty && !xfs_buf_item_dirty_format(bip)));
	ASSERT(!stale || (bip->__bli_format.blf_flags & XFS_BLF_CANCEL));

	/*
	 * Clear the buffer's association with this transaction and
	 * per-transaction state from the bli, which has been copied above.
	 */
	bp->b_transp = NULL;
	bip->bli_flags &= ~(XFS_BLI_LOGGED | XFS_BLI_HOLD | XFS_BLI_ORDERED);

	/*
	 * Unref the item and unlock the buffer unless held or stale. Stale
	 * buffers remain locked until final unpin unless the bli is freed by
	 * the unref call. The latter implies shutdown because buffer
	 * invalidation dirties the bli and transaction.
	 */
	released = xfs_buf_item_put(bip);
	if (hold || (stale && !released))
		return;
	ASSERT(!stale || aborted);
	xfs_buf_relse(bp);
}

STATIC void
xfs_buf_item_committing(
	struct xfs_log_item	*lip,
	xfs_lsn_t		commit_lsn)
{
	return xfs_buf_item_release(lip);
}

/*
 * This is called to find out where the oldest active copy of the
 * buf log item in the on disk log resides now that the last log
 * write of it completed at the given lsn.
 * We always re-log all the dirty data in a buffer, so usually the
 * latest copy in the on disk log is the only one that matters.  For
 * those cases we simply return the given lsn.
 *
 * The one exception to this is for buffers full of newly allocated
 * inodes.  These buffers are only relogged with the XFS_BLI_INODE_BUF
 * flag set, indicating that only the di_next_unlinked fields from the
 * inodes in the buffers will be replayed during recovery.  If the
 * original newly allocated inode images have not yet been flushed
 * when the buffer is so relogged, then we need to make sure that we
 * keep the old images in the 'active' portion of the log.  We do this
 * by returning the original lsn of that transaction here rather than
 * the current one.
 */
STATIC xfs_lsn_t
xfs_buf_item_committed(
	struct xfs_log_item	*lip,
	xfs_lsn_t		lsn)
{
	struct xfs_buf_log_item	*bip = BUF_ITEM(lip);

	trace_xfs_buf_item_committed(bip);

	if ((bip->bli_flags & XFS_BLI_INODE_ALLOC_BUF) && lip->li_lsn != 0)
		return lip->li_lsn;
	return lsn;
}

static const struct xfs_item_ops xfs_buf_item_ops = {
	.iop_size	= xfs_buf_item_size,
	.iop_format	= xfs_buf_item_format,
	.iop_pin	= xfs_buf_item_pin,
	.iop_unpin	= xfs_buf_item_unpin,
	.iop_release	= xfs_buf_item_release,
	.iop_committing	= xfs_buf_item_committing,
	.iop_committed	= xfs_buf_item_committed,
	.iop_push	= xfs_buf_item_push,
};

STATIC void
xfs_buf_item_get_format(
	struct xfs_buf_log_item	*bip,
	int			count)
{
	ASSERT(bip->bli_formats == NULL);
	bip->bli_format_count = count;

	if (count == 1) {
		bip->bli_formats = &bip->__bli_format;
		return;
	}

	bip->bli_formats = kmem_zalloc(count * sizeof(struct xfs_buf_log_format),
				0);
}

STATIC void
xfs_buf_item_free_format(
	struct xfs_buf_log_item	*bip)
{
	if (bip->bli_formats != &bip->__bli_format) {
		kmem_free(bip->bli_formats);
		bip->bli_formats = NULL;
	}
}

/*
 * Allocate a new buf log item to go with the given buffer.
 * Set the buffer's b_log_item field to point to the new
 * buf log item.
 */
int
xfs_buf_item_init(
	struct xfs_buf	*bp,
	struct xfs_mount *mp)
{
	struct xfs_buf_log_item	*bip = bp->b_log_item;
	int			chunks;
	int			map_size;
	int			i;

	/*
	 * Check to see if there is already a buf log item for
	 * this buffer. If we do already have one, there is
	 * nothing to do here so return.
	 */
	ASSERT(bp->b_mount == mp);
	if (bip) {
		ASSERT(bip->bli_item.li_type == XFS_LI_BUF);
		ASSERT(!bp->b_transp);
		ASSERT(bip->bli_buf == bp);
		return 0;
	}

	bip = kmem_zone_zalloc(xfs_buf_item_zone, 0);
	xfs_log_item_init(mp, &bip->bli_item, XFS_LI_BUF, &xfs_buf_item_ops);
	bip->bli_buf = bp;

	/*
	 * chunks is the number of XFS_BLF_CHUNK size pieces the buffer
	 * can be divided into. Make sure not to truncate any pieces.
	 * map_size is the size of the bitmap needed to describe the
	 * chunks of the buffer.
	 *
	 * Discontiguous buffer support follows the layout of the underlying
	 * buffer. This makes the implementation as simple as possible.
	 */
	xfs_buf_item_get_format(bip, bp->b_map_count);

	for (i = 0; i < bip->bli_format_count; i++) {
		chunks = DIV_ROUND_UP(BBTOB(bp->b_maps[i].bm_len),
				      XFS_BLF_CHUNK);
		map_size = DIV_ROUND_UP(chunks, NBWORD);

		if (map_size > XFS_BLF_DATAMAP_SIZE) {
			kmem_cache_free(xfs_buf_item_zone, bip);
			xfs_err(mp,
	"buffer item dirty bitmap (%u uints) too small to reflect %u bytes!",
					map_size,
					BBTOB(bp->b_maps[i].bm_len));
			return -EFSCORRUPTED;
		}

		bip->bli_formats[i].blf_type = XFS_LI_BUF;
		bip->bli_formats[i].blf_blkno = bp->b_maps[i].bm_bn;
		bip->bli_formats[i].blf_len = bp->b_maps[i].bm_len;
		bip->bli_formats[i].blf_map_size = map_size;
	}

	bp->b_log_item = bip;
	xfs_buf_hold(bp);
	return 0;
}


/*
 * Mark bytes first through last inclusive as dirty in the buf
 * item's bitmap.
 */
static void
xfs_buf_item_log_segment(
	uint			first,
	uint			last,
	uint			*map)
{
	uint		first_bit;
	uint		last_bit;
	uint		bits_to_set;
	uint		bits_set;
	uint		word_num;
	uint		*wordp;
	uint		bit;
	uint		end_bit;
	uint		mask;

	ASSERT(first < XFS_BLF_DATAMAP_SIZE * XFS_BLF_CHUNK * NBWORD);
	ASSERT(last < XFS_BLF_DATAMAP_SIZE * XFS_BLF_CHUNK * NBWORD);

	/*
	 * Convert byte offsets to bit numbers.
	 */
	first_bit = first >> XFS_BLF_SHIFT;
	last_bit = last >> XFS_BLF_SHIFT;

	/*
	 * Calculate the total number of bits to be set.
	 */
	bits_to_set = last_bit - first_bit + 1;

	/*
	 * Get a pointer to the first word in the bitmap
	 * to set a bit in.
	 */
	word_num = first_bit >> BIT_TO_WORD_SHIFT;
	wordp = &map[word_num];

	/*
	 * Calculate the starting bit in the first word.
	 */
	bit = first_bit & (uint)(NBWORD - 1);

	/*
	 * First set any bits in the first word of our range.
	 * If it starts at bit 0 of the word, it will be
	 * set below rather than here.  That is what the variable
	 * bit tells us. The variable bits_set tracks the number
	 * of bits that have been set so far.  End_bit is the number
	 * of the last bit to be set in this word plus one.
	 */
	if (bit) {
		end_bit = min(bit + bits_to_set, (uint)NBWORD);
		mask = ((1U << (end_bit - bit)) - 1) << bit;
		*wordp |= mask;
		wordp++;
		bits_set = end_bit - bit;
	} else {
		bits_set = 0;
	}

	/*
	 * Now set bits a whole word at a time that are between
	 * first_bit and last_bit.
	 */
	while ((bits_to_set - bits_set) >= NBWORD) {
		*wordp = 0xffffffff;
		bits_set += NBWORD;
		wordp++;
	}

	/*
	 * Finally, set any bits left to be set in one last partial word.
	 */
	end_bit = bits_to_set - bits_set;
	if (end_bit) {
		mask = (1U << end_bit) - 1;
		*wordp |= mask;
	}
}

/*
 * Mark bytes first through last inclusive as dirty in the buf
 * item's bitmap.
 */
void
xfs_buf_item_log(
	struct xfs_buf_log_item	*bip,
	uint			first,
	uint			last)
{
	int			i;
	uint			start;
	uint			end;
	struct xfs_buf		*bp = bip->bli_buf;

	/*
	 * walk each buffer segment and mark them dirty appropriately.
	 */
	start = 0;
	for (i = 0; i < bip->bli_format_count; i++) {
		if (start > last)
			break;
		end = start + BBTOB(bp->b_maps[i].bm_len) - 1;

		/* skip to the map that includes the first byte to log */
		if (first > end) {
			start += BBTOB(bp->b_maps[i].bm_len);
			continue;
		}

		/*
		 * Trim the range to this segment and mark it in the bitmap.
		 * Note that we must convert buffer offsets to segment relative
		 * offsets (e.g., the first byte of each segment is byte 0 of
		 * that segment).
		 */
		if (first < start)
			first = start;
		if (end > last)
			end = last;
		xfs_buf_item_log_segment(first - start, end - start,
					 &bip->bli_formats[i].blf_data_map[0]);

		start += BBTOB(bp->b_maps[i].bm_len);
	}
}


/*
 * Return true if the buffer has any ranges logged/dirtied by a transaction,
 * false otherwise.
 */
bool
xfs_buf_item_dirty_format(
	struct xfs_buf_log_item	*bip)
{
	int			i;

	for (i = 0; i < bip->bli_format_count; i++) {
		if (!xfs_bitmap_empty(bip->bli_formats[i].blf_data_map,
			     bip->bli_formats[i].blf_map_size))
			return true;
	}

	return false;
}

STATIC void
xfs_buf_item_free(
	struct xfs_buf_log_item	*bip)
{
	xfs_buf_item_free_format(bip);
	kmem_free(bip->bli_item.li_lv_shadow);
	kmem_cache_free(xfs_buf_item_zone, bip);
}

/*
 * This is called when the buf log item is no longer needed.  It should
 * free the buf log item associated with the given buffer and clear
 * the buffer's pointer to the buf log item.  If there are no more
 * items in the list, clear the b_iodone field of the buffer (see
 * xfs_buf_attach_iodone() below).
 */
void
xfs_buf_item_relse(
	xfs_buf_t	*bp)
{
	struct xfs_buf_log_item	*bip = bp->b_log_item;

	trace_xfs_buf_item_relse(bp, _RET_IP_);
	ASSERT(!test_bit(XFS_LI_IN_AIL, &bip->bli_item.li_flags));

	bp->b_log_item = NULL;
	if (list_empty(&bp->b_li_list))
		bp->b_iodone = NULL;

	xfs_buf_rele(bp);
	xfs_buf_item_free(bip);
}


/*
 * Add the given log item with its callback to the list of callbacks
 * to be called when the buffer's I/O completes.  If it is not set
 * already, set the buffer's b_iodone() routine to be
 * xfs_buf_iodone_callbacks() and link the log item into the list of
 * items rooted at b_li_list.
 */
void
xfs_buf_attach_iodone(
	struct xfs_buf		*bp,
	void			(*cb)(struct xfs_buf *, struct xfs_log_item *),
	struct xfs_log_item	*lip)
{
	ASSERT(xfs_buf_islocked(bp));

	lip->li_cb = cb;
	list_add_tail(&lip->li_bio_list, &bp->b_li_list);

	ASSERT(bp->b_iodone == NULL ||
	       bp->b_iodone == xfs_buf_iodone_callbacks);
	bp->b_iodone = xfs_buf_iodone_callbacks;
}

/*
 * We can have many callbacks on a buffer. Running the callbacks individually
 * can cause a lot of contention on the AIL lock, so we allow for a single
 * callback to be able to scan the remaining items in bp->b_li_list for other
 * items of the same type and callback to be processed in the first call.
 *
 * As a result, the loop walking the callback list below will also modify the
 * list. it removes the first item from the list and then runs the callback.
 * The loop then restarts from the new first item int the list. This allows the
 * callback to scan and modify the list attached to the buffer and we don't
 * have to care about maintaining a next item pointer.
 */
STATIC void
xfs_buf_do_callbacks(
	struct xfs_buf		*bp)
{
	struct xfs_buf_log_item *blip = bp->b_log_item;
	struct xfs_log_item	*lip;

	/* If there is a buf_log_item attached, run its callback */
	if (blip) {
		lip = &blip->bli_item;
		lip->li_cb(bp, lip);
	}

	while (!list_empty(&bp->b_li_list)) {
		lip = list_first_entry(&bp->b_li_list, struct xfs_log_item,
				       li_bio_list);

		/*
		 * Remove the item from the list, so we don't have any
		 * confusion if the item is added to another buf.
		 * Don't touch the log item after calling its
		 * callback, because it could have freed itself.
		 */
		list_del_init(&lip->li_bio_list);
		lip->li_cb(bp, lip);
	}
}

/*
 * Invoke the error state callback for each log item affected by the failed I/O.
 *
 * If a metadata buffer write fails with a non-permanent error, the buffer is
 * eventually resubmitted and so the completion callbacks are not run. The error
 * state may need to be propagated to the log items attached to the buffer,
 * however, so the next AIL push of the item knows hot to handle it correctly.
 */
STATIC void
xfs_buf_do_callbacks_fail(
	struct xfs_buf		*bp)
{
	struct xfs_log_item	*lip;
	struct xfs_ail		*ailp;

	/*
	 * Buffer log item errors are handled directly by xfs_buf_item_push()
	 * and xfs_buf_iodone_callback_error, and they have no IO error
	 * callbacks. Check only for items in b_li_list.
	 */
	if (list_empty(&bp->b_li_list))
		return;

	lip = list_first_entry(&bp->b_li_list, struct xfs_log_item,
			li_bio_list);
	ailp = lip->li_ailp;
	spin_lock(&ailp->ail_lock);
	list_for_each_entry(lip, &bp->b_li_list, li_bio_list) {
		if (lip->li_ops->iop_error)
			lip->li_ops->iop_error(lip, bp);
	}
	spin_unlock(&ailp->ail_lock);
}

static bool
xfs_buf_iodone_callback_error(
	struct xfs_buf		*bp)
{
	struct xfs_buf_log_item	*bip = bp->b_log_item;
	struct xfs_log_item	*lip;
	struct xfs_mount	*mp;
	static ulong		lasttime;
	static xfs_buftarg_t	*lasttarg;
	struct xfs_error_cfg	*cfg;

	/*
	 * The failed buffer might not have a buf_log_item attached or the
	 * log_item list might be empty. Get the mp from the available
	 * xfs_log_item
	 */
	lip = list_first_entry_or_null(&bp->b_li_list, struct xfs_log_item,
				       li_bio_list);
	mp = lip ? lip->li_mountp : bip->bli_item.li_mountp;

	/*
	 * If we've already decided to shutdown the filesystem because of
	 * I/O errors, there's no point in giving this a retry.
	 */
	if (XFS_FORCED_SHUTDOWN(mp))
		goto out_stale;

	if (bp->b_target != lasttarg ||
	    time_after(jiffies, (lasttime + 5*HZ))) {
		lasttime = jiffies;
		xfs_buf_ioerror_alert(bp, __this_address);
	}
	lasttarg = bp->b_target;

	/* synchronous writes will have callers process the error */
	if (!(bp->b_flags & XBF_ASYNC))
		goto out_stale;

	trace_xfs_buf_item_iodone_async(bp, _RET_IP_);
	ASSERT(bp->b_iodone != NULL);

	cfg = xfs_error_get_cfg(mp, XFS_ERR_METADATA, bp->b_error);

	/*
	 * If the write was asynchronous then no one will be looking for the
	 * error.  If this is the first failure of this type, clear the error
	 * state and write the buffer out again. This means we always retry an
	 * async write failure at least once, but we also need to set the buffer
	 * up to behave correctly now for repeated failures.
	 */
	if (!(bp->b_flags & (XBF_STALE | XBF_WRITE_FAIL)) ||
	     bp->b_last_error != bp->b_error) {
		bp->b_flags |= (XBF_WRITE | XBF_DONE | XBF_WRITE_FAIL);
		bp->b_last_error = bp->b_error;
		if (cfg->retry_timeout != XFS_ERR_RETRY_FOREVER &&
		    !bp->b_first_retry_time)
			bp->b_first_retry_time = jiffies;

		xfs_buf_ioerror(bp, 0);
		xfs_buf_submit(bp);
		return true;
	}

	/*
	 * Repeated failure on an async write. Take action according to the
	 * error configuration we have been set up to use.
	 */

	if (cfg->max_retries != XFS_ERR_RETRY_FOREVER &&
	    ++bp->b_retries > cfg->max_retries)
			goto permanent_error;
	if (cfg->retry_timeout != XFS_ERR_RETRY_FOREVER &&
	    time_after(jiffies, cfg->retry_timeout + bp->b_first_retry_time))
			goto permanent_error;

	/* At unmount we may treat errors differently */
	if ((mp->m_flags & XFS_MOUNT_UNMOUNTING) && mp->m_fail_unmount)
		goto permanent_error;

	/*
	 * Still a transient error, run IO completion failure callbacks and let
	 * the higher layers retry the buffer.
	 */
	xfs_buf_do_callbacks_fail(bp);
	xfs_buf_ioerror(bp, 0);
	xfs_buf_relse(bp);
	return true;

	/*
	 * Permanent error - we need to trigger a shutdown if we haven't already
	 * to indicate that inconsistency will result from this action.
	 */
permanent_error:
	xfs_force_shutdown(mp, SHUTDOWN_META_IO_ERROR);
out_stale:
	xfs_buf_stale(bp);
	bp->b_flags |= XBF_DONE;
	trace_xfs_buf_error_relse(bp, _RET_IP_);
	return false;
}

/*
 * This is the iodone() function for buffers which have had callbacks attached
 * to them by xfs_buf_attach_iodone(). We need to iterate the items on the
 * callback list, mark the buffer as having no more callbacks and then push the
 * buffer through IO completion processing.
 */
void
xfs_buf_iodone_callbacks(
	struct xfs_buf		*bp)
{
	/*
	 * If there is an error, process it. Some errors require us
	 * to run callbacks after failure processing is done so we
	 * detect that and take appropriate action.
	 */
	if (bp->b_error && xfs_buf_iodone_callback_error(bp))
		return;

	/*
	 * Successful IO or permanent error. Either way, we can clear the
	 * retry state here in preparation for the next error that may occur.
	 */
	bp->b_last_error = 0;
	bp->b_retries = 0;
	bp->b_first_retry_time = 0;

	xfs_buf_do_callbacks(bp);
	bp->b_log_item = NULL;
	list_del_init(&bp->b_li_list);
	bp->b_iodone = NULL;
	xfs_buf_ioend(bp);
}

/*
 * This is the iodone() function for buffers which have been
 * logged.  It is called when they are eventually flushed out.
 * It should remove the buf item from the AIL, and free the buf item.
 * It is called by xfs_buf_iodone_callbacks() above which will take
 * care of cleaning up the buffer itself.
 */
void
xfs_buf_iodone(
	struct xfs_buf		*bp,
	struct xfs_log_item	*lip)
{
	struct xfs_ail		*ailp = lip->li_ailp;

	ASSERT(BUF_ITEM(lip)->bli_buf == bp);

	xfs_buf_rele(bp);

	/*
	 * If we are forcibly shutting down, this may well be
	 * off the AIL already. That's because we simulate the
	 * log-committed callbacks to unpin these buffers. Or we may never
	 * have put this item on AIL because of the transaction was
	 * aborted forcibly. xfs_trans_ail_delete() takes care of these.
	 *
	 * Either way, AIL is useless if we're forcing a shutdown.
	 */
	spin_lock(&ailp->ail_lock);
	xfs_trans_ail_delete(ailp, lip, SHUTDOWN_CORRUPT_INCORE);
	xfs_buf_item_free(BUF_ITEM(lip));
}

/*
 * Requeue a failed buffer for writeback.
 *
 * We clear the log item failed state here as well, but we have to be careful
 * about reference counts because the only active reference counts on the buffer
 * may be the failed log items. Hence if we clear the log item failed state
 * before queuing the buffer for IO we can release all active references to
 * the buffer and free it, leading to use after free problems in
 * xfs_buf_delwri_queue. It makes no difference to the buffer or log items which
 * order we process them in - the buffer is locked, and we own the buffer list
 * so nothing on them is going to change while we are performing this action.
 *
 * Hence we can safely queue the buffer for IO before we clear the failed log
 * item state, therefore  always having an active reference to the buffer and
 * avoiding the transient zero-reference state that leads to use-after-free.
 *
 * Return true if the buffer was added to the buffer list, false if it was
 * already on the buffer list.
 */
bool
xfs_buf_resubmit_failed_buffers(
	struct xfs_buf		*bp,
	struct list_head	*buffer_list)
{
	struct xfs_log_item	*lip;
	bool			ret;

	ret = xfs_buf_delwri_queue(bp, buffer_list);

	/*
	 * XFS_LI_FAILED set/clear is protected by ail_lock, caller of this
	 * function already have it acquired
	 */
	list_for_each_entry(lip, &bp->b_li_list, li_bio_list)
		xfs_clear_li_failed(lip);

	return ret;
}

STATIC enum xlog_recover_reorder
xlog_buf_reorder_fn(
	struct xlog_recover_item	*item)
{
	struct xfs_buf_log_format	*buf_f = item->ri_buf[0].i_addr;

	if (buf_f->blf_flags & XFS_BLF_CANCEL)
		return XLOG_REORDER_CANCEL_LIST;
	if (buf_f->blf_flags & XFS_BLF_INODE_BUF)
		return XLOG_REORDER_INODE_BUFFER_LIST;
	return XLOG_REORDER_BUFFER_LIST;
}

STATIC void
xlog_recover_buffer_ra_pass2(
	struct xlog                     *log,
	struct xlog_recover_item        *item)
{
	struct xfs_buf_log_format	*buf_f = item->ri_buf[0].i_addr;
	struct xfs_mount		*mp = log->l_mp;

	if (xlog_peek_buffer_cancelled(log, buf_f->blf_blkno,
			buf_f->blf_len, buf_f->blf_flags)) {
		return;
	}

	xfs_buf_readahead(mp->m_ddev_targp, buf_f->blf_blkno,
				buf_f->blf_len, NULL);
}

/*
 * Build up the table of buf cancel records so that we don't replay
 * cancelled data in the second pass.  For buffer records that are
 * not cancel records, there is nothing to do here so we just return.
 *
 * If we get a cancel record which is already in the table, this indicates
 * that the buffer was cancelled multiple times.  In order to ensure
 * that during pass 2 we keep the record in the table until we reach its
 * last occurrence in the log, we keep a reference count in the cancel
 * record in the table to tell us how many times we expect to see this
 * record during the second pass.
 */
STATIC int
xlog_recover_buffer_pass1(
	struct xlog			*log,
	struct xlog_recover_item	*item)
{
	xfs_buf_log_format_t	*buf_f = item->ri_buf[0].i_addr;
	struct list_head	*bucket;
	struct xfs_buf_cancel	*bcp;

	if (!xfs_buf_log_check_iovec(&item->ri_buf[0])) {
		xfs_err(log->l_mp, "bad buffer log item size (%d)",
				item->ri_buf[0].i_len);
		return -EFSCORRUPTED;
	}

	/*
	 * If this isn't a cancel buffer item, then just return.
	 */
	if (!(buf_f->blf_flags & XFS_BLF_CANCEL)) {
		trace_xfs_log_recover_buf_not_cancel(log, buf_f);
		return 0;
	}

	/*
	 * Insert an xfs_buf_cancel record into the hash table of them.
	 * If there is already an identical record, bump its reference count.
	 */
	bucket = XLOG_BUF_CANCEL_BUCKET(log, buf_f->blf_blkno);
	list_for_each_entry(bcp, bucket, bc_list) {
		if (bcp->bc_blkno == buf_f->blf_blkno &&
		    bcp->bc_len == buf_f->blf_len) {
			bcp->bc_refcount++;
			trace_xfs_log_recover_buf_cancel_ref_inc(log, buf_f);
			return 0;
		}
	}

	bcp = kmem_alloc(sizeof(struct xfs_buf_cancel), 0);
	bcp->bc_blkno = buf_f->blf_blkno;
	bcp->bc_len = buf_f->blf_len;
	bcp->bc_refcount = 1;
	list_add_tail(&bcp->bc_list, bucket);

	trace_xfs_log_recover_buf_cancel_add(log, buf_f);
	return 0;
}

/*
 * Validate the recovered buffer is of the correct type and attach the
 * appropriate buffer operations to them for writeback. Magic numbers are in a
 * few places:
 *	the first 16 bits of the buffer (inode buffer, dquot buffer),
 *	the first 32 bits of the buffer (most blocks),
 *	inside a struct xfs_da_blkinfo at the start of the buffer.
 */
static void
xlog_recover_validate_buf_type(
	struct xfs_mount	*mp,
	struct xfs_buf		*bp,
	xfs_buf_log_format_t	*buf_f,
	xfs_lsn_t		current_lsn)
{
	struct xfs_da_blkinfo	*info = bp->b_addr;
	uint32_t		magic32;
	uint16_t		magic16;
	uint16_t		magicda;
	char			*warnmsg = NULL;

	/*
	 * We can only do post recovery validation on items on CRC enabled
	 * fielsystems as we need to know when the buffer was written to be able
	 * to determine if we should have replayed the item. If we replay old
	 * metadata over a newer buffer, then it will enter a temporarily
	 * inconsistent state resulting in verification failures. Hence for now
	 * just avoid the verification stage for non-crc filesystems
	 */
	if (!xfs_sb_version_hascrc(&mp->m_sb))
		return;

	magic32 = be32_to_cpu(*(__be32 *)bp->b_addr);
	magic16 = be16_to_cpu(*(__be16*)bp->b_addr);
	magicda = be16_to_cpu(info->magic);
	switch (xfs_blft_from_flags(buf_f)) {
	case XFS_BLFT_BTREE_BUF:
		switch (magic32) {
		case XFS_ABTB_CRC_MAGIC:
		case XFS_ABTB_MAGIC:
			bp->b_ops = &xfs_bnobt_buf_ops;
			break;
		case XFS_ABTC_CRC_MAGIC:
		case XFS_ABTC_MAGIC:
			bp->b_ops = &xfs_cntbt_buf_ops;
			break;
		case XFS_IBT_CRC_MAGIC:
		case XFS_IBT_MAGIC:
			bp->b_ops = &xfs_inobt_buf_ops;
			break;
		case XFS_FIBT_CRC_MAGIC:
		case XFS_FIBT_MAGIC:
			bp->b_ops = &xfs_finobt_buf_ops;
			break;
		case XFS_BMAP_CRC_MAGIC:
		case XFS_BMAP_MAGIC:
			bp->b_ops = &xfs_bmbt_buf_ops;
			break;
		case XFS_RTRMAP_CRC_MAGIC:
			bp->b_ops = &xfs_rtrmapbt_buf_ops;
			break;
		case XFS_RMAP_CRC_MAGIC:
			bp->b_ops = &xfs_rmapbt_buf_ops;
			break;
		case XFS_REFC_CRC_MAGIC:
			bp->b_ops = &xfs_refcountbt_buf_ops;
			break;
		default:
			warnmsg = "Bad btree block magic!";
			break;
		}
		break;
	case XFS_BLFT_AGF_BUF:
		if (magic32 != XFS_AGF_MAGIC) {
			warnmsg = "Bad AGF block magic!";
			break;
		}
		bp->b_ops = &xfs_agf_buf_ops;
		break;
	case XFS_BLFT_AGFL_BUF:
		if (magic32 != XFS_AGFL_MAGIC) {
			warnmsg = "Bad AGFL block magic!";
			break;
		}
		bp->b_ops = &xfs_agfl_buf_ops;
		break;
	case XFS_BLFT_AGI_BUF:
		if (magic32 != XFS_AGI_MAGIC) {
			warnmsg = "Bad AGI block magic!";
			break;
		}
		bp->b_ops = &xfs_agi_buf_ops;
		break;
	case XFS_BLFT_UDQUOT_BUF:
	case XFS_BLFT_PDQUOT_BUF:
	case XFS_BLFT_GDQUOT_BUF:
#ifdef CONFIG_XFS_QUOTA
		if (magic16 != XFS_DQUOT_MAGIC) {
			warnmsg = "Bad DQUOT block magic!";
			break;
		}
		bp->b_ops = &xfs_dquot_buf_ops;
#else
		xfs_alert(mp,
	"Trying to recover dquots without QUOTA support built in!");
		ASSERT(0);
#endif
		break;
	case XFS_BLFT_DINO_BUF:
		if (magic16 != XFS_DINODE_MAGIC) {
			warnmsg = "Bad INODE block magic!";
			break;
		}
		bp->b_ops = &xfs_inode_buf_ops;
		break;
	case XFS_BLFT_SYMLINK_BUF:
		if (magic32 != XFS_SYMLINK_MAGIC) {
			warnmsg = "Bad symlink block magic!";
			break;
		}
		bp->b_ops = &xfs_symlink_buf_ops;
		break;
	case XFS_BLFT_DIR_BLOCK_BUF:
		if (magic32 != XFS_DIR2_BLOCK_MAGIC &&
		    magic32 != XFS_DIR3_BLOCK_MAGIC) {
			warnmsg = "Bad dir block magic!";
			break;
		}
		bp->b_ops = &xfs_dir3_block_buf_ops;
		break;
	case XFS_BLFT_DIR_DATA_BUF:
		if (magic32 != XFS_DIR2_DATA_MAGIC &&
		    magic32 != XFS_DIR3_DATA_MAGIC) {
			warnmsg = "Bad dir data magic!";
			break;
		}
		bp->b_ops = &xfs_dir3_data_buf_ops;
		break;
	case XFS_BLFT_DIR_FREE_BUF:
		if (magic32 != XFS_DIR2_FREE_MAGIC &&
		    magic32 != XFS_DIR3_FREE_MAGIC) {
			warnmsg = "Bad dir3 free magic!";
			break;
		}
		bp->b_ops = &xfs_dir3_free_buf_ops;
		break;
	case XFS_BLFT_DIR_LEAF1_BUF:
		if (magicda != XFS_DIR2_LEAF1_MAGIC &&
		    magicda != XFS_DIR3_LEAF1_MAGIC) {
			warnmsg = "Bad dir leaf1 magic!";
			break;
		}
		bp->b_ops = &xfs_dir3_leaf1_buf_ops;
		break;
	case XFS_BLFT_DIR_LEAFN_BUF:
		if (magicda != XFS_DIR2_LEAFN_MAGIC &&
		    magicda != XFS_DIR3_LEAFN_MAGIC) {
			warnmsg = "Bad dir leafn magic!";
			break;
		}
		bp->b_ops = &xfs_dir3_leafn_buf_ops;
		break;
	case XFS_BLFT_DA_NODE_BUF:
		if (magicda != XFS_DA_NODE_MAGIC &&
		    magicda != XFS_DA3_NODE_MAGIC) {
			warnmsg = "Bad da node magic!";
			break;
		}
		bp->b_ops = &xfs_da3_node_buf_ops;
		break;
	case XFS_BLFT_ATTR_LEAF_BUF:
		if (magicda != XFS_ATTR_LEAF_MAGIC &&
		    magicda != XFS_ATTR3_LEAF_MAGIC) {
			warnmsg = "Bad attr leaf magic!";
			break;
		}
		bp->b_ops = &xfs_attr3_leaf_buf_ops;
		break;
	case XFS_BLFT_ATTR_RMT_BUF:
		if (magic32 != XFS_ATTR3_RMT_MAGIC) {
			warnmsg = "Bad attr remote magic!";
			break;
		}
		bp->b_ops = &xfs_attr3_rmt_buf_ops;
		break;
	case XFS_BLFT_SB_BUF:
		if (magic32 != XFS_SB_MAGIC) {
			warnmsg = "Bad SB block magic!";
			break;
		}
		bp->b_ops = &xfs_sb_buf_ops;
		break;
#ifdef CONFIG_XFS_RT
	case XFS_BLFT_RTBITMAP_BUF:
	case XFS_BLFT_RTSUMMARY_BUF:
		/* no magic numbers for verification of RT buffers */
		bp->b_ops = &xfs_rtbuf_ops;
		break;
#endif /* CONFIG_XFS_RT */
	default:
		xfs_warn(mp, "Unknown buffer type %d!",
			 xfs_blft_from_flags(buf_f));
		break;
	}

	/*
	 * Nothing else to do in the case of a NULL current LSN as this means
	 * the buffer is more recent than the change in the log and will be
	 * skipped.
	 */
	if (current_lsn == NULLCOMMITLSN)
		return;

	if (warnmsg) {
		xfs_warn(mp, warnmsg);
		ASSERT(0);
	}

	/*
	 * We must update the metadata LSN of the buffer as it is written out to
	 * ensure that older transactions never replay over this one and corrupt
	 * the buffer. This can occur if log recovery is interrupted at some
	 * point after the current transaction completes, at which point a
	 * subsequent mount starts recovery from the beginning.
	 *
	 * Write verifiers update the metadata LSN from log items attached to
	 * the buffer. Therefore, initialize a bli purely to carry the LSN to
	 * the verifier. We'll clean it up in our ->iodone() callback.
	 */
	if (bp->b_ops) {
		struct xfs_buf_log_item	*bip;

		ASSERT(!bp->b_iodone || bp->b_iodone == xlog_recover_iodone);
		bp->b_iodone = xlog_recover_iodone;
		xfs_buf_item_init(bp, mp);
		bip = bp->b_log_item;
		bip->bli_item.li_lsn = current_lsn;
	}
}

/*
 * Perform a 'normal' buffer recovery.  Each logged region of the
 * buffer should be copied over the corresponding region in the
 * given buffer.  The bitmap in the buf log format structure indicates
 * where to place the logged data.
 */
STATIC void
xlog_recover_do_reg_buffer(
	struct xfs_mount	*mp,
	xlog_recover_item_t	*item,
	struct xfs_buf		*bp,
	xfs_buf_log_format_t	*buf_f,
	xfs_lsn_t		current_lsn)
{
	int			i;
	int			bit;
	int			nbits;
	xfs_failaddr_t		fa;
	const size_t		size_disk_dquot = sizeof(struct xfs_disk_dquot);

	trace_xfs_log_recover_buf_reg_buf(mp->m_log, buf_f);

	bit = 0;
	i = 1;  /* 0 is the buf format structure */
	while (1) {
		bit = xfs_next_bit(buf_f->blf_data_map,
				   buf_f->blf_map_size, bit);
		if (bit == -1)
			break;
		nbits = xfs_contig_bits(buf_f->blf_data_map,
					buf_f->blf_map_size, bit);
		ASSERT(nbits > 0);
		ASSERT(item->ri_buf[i].i_addr != NULL);
		ASSERT(item->ri_buf[i].i_len % XFS_BLF_CHUNK == 0);
		ASSERT(BBTOB(bp->b_length) >=
		       ((uint)bit << XFS_BLF_SHIFT) + (nbits << XFS_BLF_SHIFT));

		/*
		 * The dirty regions logged in the buffer, even though
		 * contiguous, may span multiple chunks. This is because the
		 * dirty region may span a physical page boundary in a buffer
		 * and hence be split into two separate vectors for writing into
		 * the log. Hence we need to trim nbits back to the length of
		 * the current region being copied out of the log.
		 */
		if (item->ri_buf[i].i_len < (nbits << XFS_BLF_SHIFT))
			nbits = item->ri_buf[i].i_len >> XFS_BLF_SHIFT;

		/*
		 * Do a sanity check if this is a dquot buffer. Just checking
		 * the first dquot in the buffer should do. XXXThis is
		 * probably a good thing to do for other buf types also.
		 */
		fa = NULL;
		if (buf_f->blf_flags &
		   (XFS_BLF_UDQUOT_BUF|XFS_BLF_PDQUOT_BUF|XFS_BLF_GDQUOT_BUF)) {
			if (item->ri_buf[i].i_addr == NULL) {
				xfs_alert(mp,
					"XFS: NULL dquot in %s.", __func__);
				goto next;
			}
			if (item->ri_buf[i].i_len < size_disk_dquot) {
				xfs_alert(mp,
					"XFS: dquot too small (%d) in %s.",
					item->ri_buf[i].i_len, __func__);
				goto next;
			}
			fa = xfs_dquot_verify(mp, item->ri_buf[i].i_addr,
					       -1, 0);
			if (fa) {
				xfs_alert(mp,
	"dquot corrupt at %pS trying to replay into block 0x%llx",
					fa, bp->b_bn);
				goto next;
			}
		}

		memcpy(xfs_buf_offset(bp,
			(uint)bit << XFS_BLF_SHIFT),	/* dest */
			item->ri_buf[i].i_addr,		/* source */
			nbits<<XFS_BLF_SHIFT);		/* length */
 next:
		i++;
		bit += nbits;
	}

	/* Shouldn't be any more regions */
	ASSERT(i == item->ri_total);

	xlog_recover_validate_buf_type(mp, bp, buf_f, current_lsn);
}

/*
 * Perform a dquot buffer recovery.
 * Simple algorithm: if we have found a QUOTAOFF log item of the same type
 * (ie. USR or GRP), then just toss this buffer away; don't recover it.
 * Else, treat it as a regular buffer and do recovery.
 *
 * Return false if the buffer was tossed and true if we recovered the buffer to
 * indicate to the caller if the buffer needs writing.
 */
STATIC bool
xlog_recover_do_dquot_buffer(
	struct xfs_mount		*mp,
	struct xlog			*log,
	struct xlog_recover_item	*item,
	struct xfs_buf			*bp,
	struct xfs_buf_log_format	*buf_f)
{
	uint			type;

	trace_xfs_log_recover_buf_dquot_buf(log, buf_f);

	/*
	 * Filesystems are required to send in quota flags at mount time.
	 */
	if (!mp->m_qflags)
		return false;

	type = 0;
	if (buf_f->blf_flags & XFS_BLF_UDQUOT_BUF)
		type |= XFS_DQ_USER;
	if (buf_f->blf_flags & XFS_BLF_PDQUOT_BUF)
		type |= XFS_DQ_PROJ;
	if (buf_f->blf_flags & XFS_BLF_GDQUOT_BUF)
		type |= XFS_DQ_GROUP;
	/*
	 * This type of quotas was turned off, so ignore this buffer
	 */
	if (log->l_quotaoffs_flag & type)
		return false;

	xlog_recover_do_reg_buffer(mp, item, bp, buf_f, NULLCOMMITLSN);
	return true;
}

/*
 * Perform recovery for a buffer full of inodes.  In these buffers, the only
 * data which should be recovered is that which corresponds to the
 * di_next_unlinked pointers in the on disk inode structures.  The rest of the
 * data for the inodes is always logged through the inodes themselves rather
 * than the inode buffer and is recovered in xlog_recover_inode_pass2().
 *
 * The only time when buffers full of inodes are fully recovered is when the
 * buffer is full of newly allocated inodes.  In this case the buffer will
 * not be marked as an inode buffer and so will be sent to
 * xlog_recover_do_reg_buffer() below during recovery.
 */
STATIC int
xlog_recover_do_inode_buffer(
	struct xfs_mount	*mp,
	xlog_recover_item_t	*item,
	struct xfs_buf		*bp,
	xfs_buf_log_format_t	*buf_f)
{
	int			i;
	int			item_index = 0;
	int			bit = 0;
	int			nbits = 0;
	int			reg_buf_offset = 0;
	int			reg_buf_bytes = 0;
	int			next_unlinked_offset;
	int			inodes_per_buf;
	xfs_agino_t		*logged_nextp;
	xfs_agino_t		*buffer_nextp;

	trace_xfs_log_recover_buf_inode_buf(mp->m_log, buf_f);

	/*
	 * Post recovery validation only works properly on CRC enabled
	 * filesystems.
	 */
	if (xfs_sb_version_hascrc(&mp->m_sb))
		bp->b_ops = &xfs_inode_buf_ops;

	inodes_per_buf = BBTOB(bp->b_length) >> mp->m_sb.sb_inodelog;
	for (i = 0; i < inodes_per_buf; i++) {
		next_unlinked_offset = (i * mp->m_sb.sb_inodesize) +
			offsetof(xfs_dinode_t, di_next_unlinked);

		while (next_unlinked_offset >=
		       (reg_buf_offset + reg_buf_bytes)) {
			/*
			 * The next di_next_unlinked field is beyond
			 * the current logged region.  Find the next
			 * logged region that contains or is beyond
			 * the current di_next_unlinked field.
			 */
			bit += nbits;
			bit = xfs_next_bit(buf_f->blf_data_map,
					   buf_f->blf_map_size, bit);

			/*
			 * If there are no more logged regions in the
			 * buffer, then we're done.
			 */
			if (bit == -1)
				return 0;

			nbits = xfs_contig_bits(buf_f->blf_data_map,
						buf_f->blf_map_size, bit);
			ASSERT(nbits > 0);
			reg_buf_offset = bit << XFS_BLF_SHIFT;
			reg_buf_bytes = nbits << XFS_BLF_SHIFT;
			item_index++;
		}

		/*
		 * If the current logged region starts after the current
		 * di_next_unlinked field, then move on to the next
		 * di_next_unlinked field.
		 */
		if (next_unlinked_offset < reg_buf_offset)
			continue;

		ASSERT(item->ri_buf[item_index].i_addr != NULL);
		ASSERT((item->ri_buf[item_index].i_len % XFS_BLF_CHUNK) == 0);
		ASSERT((reg_buf_offset + reg_buf_bytes) <= BBTOB(bp->b_length));

		/*
		 * The current logged region contains a copy of the
		 * current di_next_unlinked field.  Extract its value
		 * and copy it to the buffer copy.
		 */
		logged_nextp = item->ri_buf[item_index].i_addr +
				next_unlinked_offset - reg_buf_offset;
		if (XFS_IS_CORRUPT(mp, *logged_nextp == 0)) {
			xfs_alert(mp,
		"Bad inode buffer log record (ptr = "PTR_FMT", bp = "PTR_FMT"). "
		"Trying to replay bad (0) inode di_next_unlinked field.",
				item, bp);
			return -EFSCORRUPTED;
		}

		buffer_nextp = xfs_buf_offset(bp, next_unlinked_offset);
		*buffer_nextp = *logged_nextp;

		/*
		 * If necessary, recalculate the CRC in the on-disk inode. We
		 * have to leave the inode in a consistent state for whoever
		 * reads it next....
		 */
		xfs_dinode_calc_crc(mp,
				xfs_buf_offset(bp, i * mp->m_sb.sb_inodesize));

	}

	return 0;
}

/*
 * V5 filesystems know the age of the buffer on disk being recovered. We can
 * have newer objects on disk than we are replaying, and so for these cases we
 * don't want to replay the current change as that will make the buffer contents
 * temporarily invalid on disk.
 *
 * The magic number might not match the buffer type we are going to recover
 * (e.g. reallocated blocks), so we ignore the xfs_buf_log_format flags.  Hence
 * extract the LSN of the existing object in the buffer based on it's current
 * magic number.  If we don't recognise the magic number in the buffer, then
 * return a LSN of -1 so that the caller knows it was an unrecognised block and
 * so can recover the buffer.
 *
 * Note: we cannot rely solely on magic number matches to determine that the
 * buffer has a valid LSN - we also need to verify that it belongs to this
 * filesystem, so we need to extract the object's LSN and compare it to that
 * which we read from the superblock. If the UUIDs don't match, then we've got a
 * stale metadata block from an old filesystem instance that we need to recover
 * over the top of.
 */
static xfs_lsn_t
xlog_recover_get_buf_lsn(
	struct xfs_mount	*mp,
	struct xfs_buf		*bp)
{
	uint32_t		magic32;
	uint16_t		magic16;
	uint16_t		magicda;
	void			*blk = bp->b_addr;
	uuid_t			*uuid;
	xfs_lsn_t		lsn = -1;

	/* v4 filesystems always recover immediately */
	if (!xfs_sb_version_hascrc(&mp->m_sb))
		goto recover_immediately;

	magic32 = be32_to_cpu(*(__be32 *)blk);
	switch (magic32) {
	case XFS_ABTB_CRC_MAGIC:
	case XFS_ABTC_CRC_MAGIC:
	case XFS_ABTB_MAGIC:
	case XFS_ABTC_MAGIC:
	case XFS_RMAP_CRC_MAGIC:
	case XFS_REFC_CRC_MAGIC:
	case XFS_IBT_CRC_MAGIC:
	case XFS_IBT_MAGIC: {
		struct xfs_btree_block *btb = blk;

		lsn = be64_to_cpu(btb->bb_u.s.bb_lsn);
		uuid = &btb->bb_u.s.bb_uuid;
		break;
	}
	case XFS_RTRMAP_CRC_MAGIC:
	case XFS_BMAP_CRC_MAGIC:
	case XFS_BMAP_MAGIC: {
		struct xfs_btree_block *btb = blk;

		lsn = be64_to_cpu(btb->bb_u.l.bb_lsn);
		uuid = &btb->bb_u.l.bb_uuid;
		break;
	}
	case XFS_AGF_MAGIC:
		lsn = be64_to_cpu(((struct xfs_agf *)blk)->agf_lsn);
		uuid = &((struct xfs_agf *)blk)->agf_uuid;
		break;
	case XFS_AGFL_MAGIC:
		lsn = be64_to_cpu(((struct xfs_agfl *)blk)->agfl_lsn);
		uuid = &((struct xfs_agfl *)blk)->agfl_uuid;
		break;
	case XFS_AGI_MAGIC:
		lsn = be64_to_cpu(((struct xfs_agi *)blk)->agi_lsn);
		uuid = &((struct xfs_agi *)blk)->agi_uuid;
		break;
	case XFS_SYMLINK_MAGIC:
		lsn = be64_to_cpu(((struct xfs_dsymlink_hdr *)blk)->sl_lsn);
		uuid = &((struct xfs_dsymlink_hdr *)blk)->sl_uuid;
		break;
	case XFS_DIR3_BLOCK_MAGIC:
	case XFS_DIR3_DATA_MAGIC:
	case XFS_DIR3_FREE_MAGIC:
		lsn = be64_to_cpu(((struct xfs_dir3_blk_hdr *)blk)->lsn);
		uuid = &((struct xfs_dir3_blk_hdr *)blk)->uuid;
		break;
	case XFS_ATTR3_RMT_MAGIC:
		/*
		 * Remote attr blocks are written synchronously, rather than
		 * being logged. That means they do not contain a valid LSN
		 * (i.e. transactionally ordered) in them, and hence any time we
		 * see a buffer to replay over the top of a remote attribute
		 * block we should simply do so.
		 */
		goto recover_immediately;
	case XFS_SB_MAGIC:
		/*
		 * superblock uuids are magic. We may or may not have a
		 * sb_meta_uuid on disk, but it will be set in the in-core
		 * superblock. We set the uuid pointer for verification
		 * according to the superblock feature mask to ensure we check
		 * the relevant UUID in the superblock.
		 */
		lsn = be64_to_cpu(((struct xfs_dsb *)blk)->sb_lsn);
		if (xfs_sb_version_hasmetauuid(&mp->m_sb))
			uuid = &((struct xfs_dsb *)blk)->sb_meta_uuid;
		else
			uuid = &((struct xfs_dsb *)blk)->sb_uuid;
		break;
	default:
		break;
	}

	if (lsn != (xfs_lsn_t)-1) {
		if (!uuid_equal(&mp->m_sb.sb_meta_uuid, uuid))
			goto recover_immediately;
		return lsn;
	}

	magicda = be16_to_cpu(((struct xfs_da_blkinfo *)blk)->magic);
	switch (magicda) {
	case XFS_DIR3_LEAF1_MAGIC:
	case XFS_DIR3_LEAFN_MAGIC:
	case XFS_DA3_NODE_MAGIC:
		lsn = be64_to_cpu(((struct xfs_da3_blkinfo *)blk)->lsn);
		uuid = &((struct xfs_da3_blkinfo *)blk)->uuid;
		break;
	default:
		break;
	}

	if (lsn != (xfs_lsn_t)-1) {
		if (!uuid_equal(&mp->m_sb.sb_uuid, uuid))
			goto recover_immediately;
		return lsn;
	}

	/*
	 * We do individual object checks on dquot and inode buffers as they
	 * have their own individual LSN records. Also, we could have a stale
	 * buffer here, so we have to at least recognise these buffer types.
	 *
	 * A notd complexity here is inode unlinked list processing - it logs
	 * the inode directly in the buffer, but we don't know which inodes have
	 * been modified, and there is no global buffer LSN. Hence we need to
	 * recover all inode buffer types immediately. This problem will be
	 * fixed by logical logging of the unlinked list modifications.
	 */
	magic16 = be16_to_cpu(*(__be16 *)blk);
	switch (magic16) {
	case XFS_DQUOT_MAGIC:
	case XFS_DINODE_MAGIC:
		goto recover_immediately;
	default:
		break;
	}

	/* unknown buffer contents, recover immediately */

recover_immediately:
	return (xfs_lsn_t)-1;

}

/*
 * This routine replays a modification made to a buffer at runtime.
 * There are actually two types of buffer, regular and inode, which
 * are handled differently.  Inode buffers are handled differently
 * in that we only recover a specific set of data from them, namely
 * the inode di_next_unlinked fields.  This is because all other inode
 * data is actually logged via inode records and any data we replay
 * here which overlaps that may be stale.
 *
 * When meta-data buffers are freed at run time we log a buffer item
 * with the XFS_BLF_CANCEL bit set to indicate that previous copies
 * of the buffer in the log should not be replayed at recovery time.
 * This is so that if the blocks covered by the buffer are reused for
 * file data before we crash we don't end up replaying old, freed
 * meta-data into a user's file.
 *
 * To handle the cancellation of buffer log items, we make two passes
 * over the log during recovery.  During the first we build a table of
 * those buffers which have been cancelled, and during the second we
 * only replay those buffers which do not have corresponding cancel
 * records in the table.  See xlog_recover_buffer_pass[1,2] above
 * for more details on the implementation of the table of cancel records.
 */
STATIC int
xlog_recover_buffer_pass2(
	struct xlog			*log,
	struct list_head		*buffer_list,
	struct xlog_recover_item	*item,
	xfs_lsn_t			current_lsn)
{
	xfs_buf_log_format_t	*buf_f = item->ri_buf[0].i_addr;
	xfs_mount_t		*mp = log->l_mp;
	xfs_buf_t		*bp;
	int			error;
	uint			buf_flags;
	xfs_lsn_t		lsn;

	/*
	 * In this pass we only want to recover all the buffers which have
	 * not been cancelled and are not cancellation buffers themselves.
	 */
	if (xlog_check_buffer_cancelled(log, buf_f->blf_blkno,
			buf_f->blf_len, buf_f->blf_flags)) {
		trace_xfs_log_recover_buf_cancel(log, buf_f);
		return 0;
	}

	trace_xfs_log_recover_buf_recover(log, buf_f);

	buf_flags = 0;
	if (buf_f->blf_flags & XFS_BLF_INODE_BUF)
		buf_flags |= XBF_UNMAPPED;

	error = xfs_buf_read(mp->m_ddev_targp, buf_f->blf_blkno, buf_f->blf_len,
			  buf_flags, &bp, NULL);
	if (error)
		return error;

	/*
	 * Recover the buffer only if we get an LSN from it and it's less than
	 * the lsn of the transaction we are replaying.
	 *
	 * Note that we have to be extremely careful of readahead here.
	 * Readahead does not attach verfiers to the buffers so if we don't
	 * actually do any replay after readahead because of the LSN we found
	 * in the buffer if more recent than that current transaction then we
	 * need to attach the verifier directly. Failure to do so can lead to
	 * future recovery actions (e.g. EFI and unlinked list recovery) can
	 * operate on the buffers and they won't get the verifier attached. This
	 * can lead to blocks on disk having the correct content but a stale
	 * CRC.
	 *
	 * It is safe to assume these clean buffers are currently up to date.
	 * If the buffer is dirtied by a later transaction being replayed, then
	 * the verifier will be reset to match whatever recover turns that
	 * buffer into.
	 */
	lsn = xlog_recover_get_buf_lsn(mp, bp);
	if (lsn && lsn != -1 && XFS_LSN_CMP(lsn, current_lsn) >= 0) {
		trace_xfs_log_recover_buf_skip(log, buf_f);
		xlog_recover_validate_buf_type(mp, bp, buf_f, NULLCOMMITLSN);
		goto out_release;
	}

	if (buf_f->blf_flags & XFS_BLF_INODE_BUF) {
		error = xlog_recover_do_inode_buffer(mp, item, bp, buf_f);
		if (error)
			goto out_release;
	} else if (buf_f->blf_flags &
		  (XFS_BLF_UDQUOT_BUF|XFS_BLF_PDQUOT_BUF|XFS_BLF_GDQUOT_BUF)) {
		bool	dirty;

		dirty = xlog_recover_do_dquot_buffer(mp, log, item, bp, buf_f);
		if (!dirty)
			goto out_release;
	} else {
		xlog_recover_do_reg_buffer(mp, item, bp, buf_f, current_lsn);
	}

	/*
	 * Perform delayed write on the buffer.  Asynchronous writes will be
	 * slower when taking into account all the buffers to be flushed.
	 *
	 * Also make sure that only inode buffers with good sizes stay in
	 * the buffer cache.  The kernel moves inodes in buffers of 1 block
	 * or inode_cluster_size bytes, whichever is bigger.  The inode
	 * buffers in the log can be a different size if the log was generated
	 * by an older kernel using unclustered inode buffers or a newer kernel
	 * running with a different inode cluster size.  Regardless, if the
	 * the inode buffer size isn't max(blocksize, inode_cluster_size)
	 * for *our* value of inode_cluster_size, then we need to keep
	 * the buffer out of the buffer cache so that the buffer won't
	 * overlap with future reads of those inodes.
	 */
	if (XFS_DINODE_MAGIC ==
	    be16_to_cpu(*((__be16 *)xfs_buf_offset(bp, 0))) &&
	    (BBTOB(bp->b_length) != M_IGEO(log->l_mp)->inode_cluster_size)) {
		xfs_buf_stale(bp);
		error = xfs_bwrite(bp);
	} else {
		ASSERT(bp->b_mount == mp);
		bp->b_iodone = xlog_recover_iodone;
		xfs_buf_delwri_queue(bp, buffer_list);
	}

out_release:
	xfs_buf_relse(bp);
	return error;
}

const struct xlog_recover_item_type xlog_buf_item_type = {
	.reorder_fn		= xlog_buf_reorder_fn,
	.ra_pass2_fn		= xlog_recover_buffer_ra_pass2,
	.commit_pass1_fn	= xlog_recover_buffer_pass1,
	.commit_pass2_fn	= xlog_recover_buffer_pass2,
};
