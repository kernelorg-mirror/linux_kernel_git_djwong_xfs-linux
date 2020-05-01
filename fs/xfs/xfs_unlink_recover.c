// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
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
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_log_recover.h"
#include "xfs_trans_priv.h"
#include "xfs_ialloc.h"
#include "xfs_icache.h"

/*
 * This routine performs a transaction to null out a bad inode pointer
 * in an agi unlinked inode hash bucket.
 */
STATIC void
xlog_recover_clear_agi_bucket(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	int			bucket)
{
	struct xfs_trans	*tp;
	struct xfs_agi		*agi;
	struct xfs_buf		*agibp;
	int			offset;
	int			error;

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_clearagi, 0, 0, 0, &tp);
	if (error)
		goto out_error;

	error = xfs_read_agi(mp, tp, agno, &agibp);
	if (error)
		goto out_abort;

	agi = agibp->b_addr;
	agi->agi_unlinked[bucket] = cpu_to_be32(NULLAGINO);
	offset = offsetof(xfs_agi_t, agi_unlinked) +
		 (sizeof(xfs_agino_t) * bucket);
	xfs_trans_log_buf(tp, agibp, offset,
			  (offset + sizeof(xfs_agino_t) - 1));

	error = xfs_trans_commit(tp);
	if (error)
		goto out_error;
	return;

out_abort:
	xfs_trans_cancel(tp);
out_error:
	xfs_warn(mp, "%s: failed to clear agi %d. Continuing.", __func__, agno);
	return;
}

STATIC xfs_agino_t
xlog_recover_process_one_iunlink(
	struct xfs_mount		*mp,
	xfs_agnumber_t			agno,
	xfs_agino_t			agino,
	int				bucket)
{
	struct xfs_buf			*ibp;
	struct xfs_dinode		*dip;
	struct xfs_inode		*ip;
	xfs_ino_t			ino;
	int				error;

	ino = XFS_AGINO_TO_INO(mp, agno, agino);
	error = xfs_iget(mp, NULL, ino, 0, 0, &ip);
	if (error)
		goto fail;

	/*
	 * Get the on disk inode to find the next inode in the bucket.
	 */
	error = xfs_imap_to_bp(mp, NULL, &ip->i_imap, &dip, &ibp, 0, 0);
	if (error)
		goto fail_iput;

	xfs_iflags_clear(ip, XFS_IRECOVERY);
	ASSERT(VFS_I(ip)->i_nlink == 0);
	ASSERT(VFS_I(ip)->i_mode != 0);

	/* setup for the next pass */
	agino = be32_to_cpu(dip->di_next_unlinked);
	xfs_buf_relse(ibp);

	/*
	 * Prevent any DMAPI event from being sent when the reference on
	 * the inode is dropped.
	 */
	ip->i_d.di_dmevmask = 0;

	xfs_irele(ip);
	return agino;

 fail_iput:
	xfs_irele(ip);
 fail:
	/*
	 * We can't read in the inode this bucket points to, or this inode
	 * is messed up.  Just ditch this bucket of inodes.  We will lose
	 * some inodes and space, but at least we won't hang.
	 *
	 * Call xlog_recover_clear_agi_bucket() to perform a transaction to
	 * clear the inode pointer in the bucket.
	 */
	xlog_recover_clear_agi_bucket(mp, agno, bucket);
	return NULLAGINO;
}

/*
 * Recover AGI unlinked lists
 *
 * This is called during recovery to process any inodes which we unlinked but
 * not freed when the system crashed.  These inodes will be on the lists in the
 * AGI blocks. What we do here is scan all the AGIs and fully truncate and free
 * any inodes found on the lists. Each inode is removed from the lists when it
 * has been fully truncated and is freed. The freeing of the inode and its
 * removal from the list must be atomic.
 *
 * If everything we touch in the agi processing loop is already in memory, this
 * loop can hold the cpu for a long time. It runs without lock contention,
 * memory allocation contention, the need wait for IO, etc, and so will run
 * until we either run out of inodes to process, run low on memory or we run out
 * of log space.
 *
 * This behaviour is bad for latency on single CPU and non-preemptible kernels,
 * and can prevent other filesytem work (such as CIL pushes) from running. This
 * can lead to deadlocks if the recovery process runs out of log reservation
 * space. Hence we need to yield the CPU when there is other kernel work
 * scheduled on this CPU to ensure other scheduled work can run without undue
 * latency.
 */
STATIC int
xlog_recover_process_iunlinked(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno)
{
	struct xfs_agi		*agi;
	struct xfs_buf		*agibp;
	xfs_agino_t		agino;
	int			bucket;
	int			error;

	/*
	 * Find the agi for this ag.
	 */
	error = xfs_read_agi(mp, NULL, agno, &agibp);
	if (error) {
		/*
		 * AGI is b0rked. Don't process it.
		 *
		 * We should probably mark the filesystem as corrupt
		 * after we've recovered all the ag's we can....
		 */
		return error;
	}

	/*
	 * Unlock the buffer so that it can be acquired in the normal
	 * course of the transaction to truncate and free each inode.
	 * Because we are not racing with anyone else here for the AGI
	 * buffer, we don't even need to hold it locked to read the
	 * initial unlinked bucket entries out of the buffer. We keep
	 * buffer reference though, so that it stays pinned in memory
	 * while we need the buffer.
	 */
	agi = agibp->b_addr;
	xfs_buf_unlock(agibp);

	for (bucket = 0; bucket < XFS_AGI_UNLINKED_BUCKETS; bucket++) {
		agino = be32_to_cpu(agi->agi_unlinked[bucket]);
		while (agino != NULLAGINO) {
			agino = xlog_recover_process_one_iunlink(mp,
						agno, agino, bucket);
			cond_resched();
		}
	}
	xfs_buf_rele(agibp);

	return 0;
}

int
xlog_recover_process_unlinked(
	struct xlog		*log)
{
	struct xfs_mount	*mp = log->l_mp;
	xfs_agnumber_t		agno;
	int			error;

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		error = xlog_recover_process_iunlinked(mp, agno);
		if (error)
			break;
	}

	return error;
}
