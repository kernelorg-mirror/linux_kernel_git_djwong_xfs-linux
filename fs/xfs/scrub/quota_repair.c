// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_btree.h"
#include "xfs_bit.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_inode.h"
#include "xfs_inode_fork.h"
#include "xfs_alloc.h"
#include "xfs_bmap.h"
#include "xfs_quota.h"
#include "xfs_qm.h"
#include "xfs_dquot.h"
#include "xfs_dquot_item.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"

/*
 * Quota Repair
 * ============
 *
 * Quota repairs are fairly simplistic; we fix everything that the dquot
 * verifiers complain about, cap any counters or limits that make no sense,
 * and schedule a quotacheck if we had to fix anything.  We also repair any
 * data fork extent records that don't apply to metadata files.
 */

struct xrep_quota_info {
	struct xfs_scrub	*sc;
	bool			need_quotacheck;
};

/* Scrub the fields in an individual quota item. */
STATIC int
xrep_quota_item(
	struct xfs_dquot	*dqp,
	uint			dqtype,
	void			*priv)
{
	struct xrep_quota_info	*rqi = priv;
	struct xfs_scrub	*sc = rqi->sc;
	struct xfs_mount	*mp = sc->mp;
	xfs_ino_t		fs_icount;
	bool			dirty = false;
	int			error;

	/* Check the limits. */
	if (dqp->q_blk_softlimit > dqp->q_blk_hardlimit) {
		dqp->q_blk_softlimit = dqp->q_blk_hardlimit;
		dirty = true;
	}

	if (dqp->q_ino_softlimit > dqp->q_ino_hardlimit) {
		dqp->q_ino_softlimit = dqp->q_ino_hardlimit;
		dirty = true;
	}

	if (dqp->q_rtb_softlimit > dqp->q_rtb_hardlimit) {
		dqp->q_rtb_softlimit = dqp->q_rtb_hardlimit;
		dirty = true;
	}

	/*
	 * Check that usage doesn't exceed physical limits.  However, on
	 * a reflink filesystem we're allowed to exceed physical space
	 * if there are no quota limits.  We don't know what the real number
	 * is, but we can make quotacheck find out for us.
	 */
	if (!xfs_sb_version_hasreflink(&mp->m_sb) &&
	    dqp->q_bcount > mp->m_sb.sb_dblocks) {
		dqp->q_res_bcount -= dqp->q_bcount;
		dqp->q_res_bcount += mp->m_sb.sb_dblocks;
		dqp->q_bcount = mp->m_sb.sb_dblocks;
		rqi->need_quotacheck = true;
		dirty = true;
	}
	fs_icount = percpu_counter_sum(&mp->m_icount);
	if (dqp->q_icount > fs_icount) {
		dqp->q_res_icount -= dqp->q_icount;
		dqp->q_res_icount += fs_icount;
		dqp->q_icount = fs_icount;
		rqi->need_quotacheck = true;
		dirty = true;
	}
	if (dqp->q_rtbcount > mp->m_sb.sb_rblocks) {
		dqp->q_res_rtbcount -= dqp->q_rtbcount;
		dqp->q_res_rtbcount += mp->m_sb.sb_rblocks;
		dqp->q_rtbcount = mp->m_sb.sb_rblocks;
		rqi->need_quotacheck = true;
		dirty = true;
	}

	if (!dirty)
		return 0;

	dqp->dq_flags |= XFS_DQ_DIRTY;
	xfs_trans_dqjoin(sc->tp, dqp);
	if (dqp->q_id) {
		xfs_qm_adjust_dqlimits(dqp);
		xfs_qm_adjust_dqtimers(dqp);
	}
	xfs_trans_log_dquot(sc->tp, dqp);
	error = xfs_trans_roll(&sc->tp);
	xfs_dqlock(dqp);
	return error;
}

/* Fix a quota timer so that we can pass the verifier. */
STATIC void
xrep_quota_fix_timer(
	__be64			softlimit,
	__be64			countnow,
	__be32			*timer,
	time64_t		timelimit)
{
	uint64_t		soft = be64_to_cpu(softlimit);
	uint64_t		count = be64_to_cpu(countnow);

	if (soft && count > soft && *timer == 0)
		*timer = cpu_to_be32(ktime_get_real_seconds() + timelimit);
}

/* Fix anything the verifiers complain about. */
STATIC int
xrep_quota_block(
	struct xfs_scrub	*sc,
	struct xfs_buf		*bp,
	uint			dqtype,
	xfs_dqid_t		id)
{
	struct xfs_dqblk	*d = (struct xfs_dqblk *)bp->b_addr;
	struct xfs_disk_dquot	*ddq;
	struct xfs_quotainfo	*qi = sc->mp->m_quotainfo;
	struct xfs_def_quota	*defq = xfs_get_defquota(qi, dqtype);
	enum xfs_blft		buftype = 0;
	int			i;

	bp->b_ops = &xfs_dquot_buf_ops;
	for (i = 0; i < qi->qi_dqperchunk; i++) {
		ddq = &d[i].dd_diskdq;

		ddq->d_magic = cpu_to_be16(XFS_DQUOT_MAGIC);
		ddq->d_version = XFS_DQUOT_VERSION;
		ddq->d_flags = dqtype;
		ddq->d_id = cpu_to_be32(id + i);

		xrep_quota_fix_timer(ddq->d_blk_softlimit,
				ddq->d_bcount, &ddq->d_btimer,
				defq->btimelimit);

		xrep_quota_fix_timer(ddq->d_ino_softlimit,
				ddq->d_icount, &ddq->d_itimer,
				defq->itimelimit);

		xrep_quota_fix_timer(ddq->d_rtb_softlimit,
				ddq->d_rtbcount, &ddq->d_rtbtimer,
				defq->rtbtimelimit);

		/* We only support v5 filesystems so always set these. */
		uuid_copy(&d->dd_uuid, &sc->mp->m_sb.sb_meta_uuid);
		xfs_update_cksum((char *)d, sizeof(struct xfs_dqblk),
				 XFS_DQUOT_CRC_OFF);
		d->dd_lsn = 0;
	}
	switch (dqtype) {
	case XFS_DQ_USER:
		buftype = XFS_BLFT_UDQUOT_BUF;
		break;
	case XFS_DQ_GROUP:
		buftype = XFS_BLFT_GDQUOT_BUF;
		break;
	case XFS_DQ_PROJ:
		buftype = XFS_BLFT_PDQUOT_BUF;
		break;
	}
	xfs_trans_buf_set_type(sc->tp, bp, buftype);
	xfs_trans_log_buf(sc->tp, bp, 0, BBTOB(bp->b_length) - 1);
	return xrep_roll_trans(sc);
}

/*
 * Repair a quota file's data fork.  The function returns with the inode
 * joined.
 */
STATIC int
xrep_quota_data_fork(
	struct xfs_scrub	*sc,
	uint			dqtype)
{
	struct xfs_bmbt_irec	irec = { 0 };
	struct xfs_iext_cursor	icur;
	struct xfs_quotainfo	*qi = sc->mp->m_quotainfo;
	struct xfs_ifork	*ifp;
	struct xfs_buf		*bp;
	struct xfs_dqblk	*d;
	xfs_dqid_t		id;
	xfs_fileoff_t		max_dqid_off;
	xfs_fileoff_t		off;
	xfs_fsblock_t		fsbno;
	bool			truncate = false;
	int			error = 0;

	error = xrep_metadata_inode_forks(sc);
	if (error)
		goto out;

	/* Check for data fork problems that apply only to quota files. */
	max_dqid_off = ((xfs_dqid_t)-1) / qi->qi_dqperchunk;
	ifp = XFS_IFORK_PTR(sc->ip, XFS_DATA_FORK);
	for_each_xfs_iext(ifp, &icur, &irec) {
		if (isnullstartblock(irec.br_startblock)) {
			error = -EFSCORRUPTED;
			goto out;
		}

		if (irec.br_startoff > max_dqid_off ||
		    irec.br_startoff + irec.br_blockcount - 1 > max_dqid_off) {
			truncate = true;
			break;
		}
	}

	xfs_trans_ijoin(sc->tp, sc->ip, 0);

	if (truncate) {
		error = xfs_itruncate_extents(&sc->tp, sc->ip, XFS_DATA_FORK,
				max_dqid_off * sc->mp->m_sb.sb_blocksize);
		if (error)
			goto out;
	}

	/* Now go fix anything that fails the verifiers. */
	for_each_xfs_iext(ifp, &icur, &irec) {
		for (fsbno = irec.br_startblock, off = irec.br_startoff;
		     fsbno < irec.br_startblock + irec.br_blockcount;
		     fsbno += XFS_DQUOT_CLUSTER_SIZE_FSB,
				off += XFS_DQUOT_CLUSTER_SIZE_FSB) {
			id = off * qi->qi_dqperchunk;
			error = xfs_trans_read_buf(sc->mp, sc->tp,
					sc->mp->m_ddev_targp,
					XFS_FSB_TO_DADDR(sc->mp, fsbno),
					qi->qi_dqchunklen,
					0, &bp, &xfs_dquot_buf_ops);
			if (error == 0) {
				d = (struct xfs_dqblk *)bp->b_addr;
				if (id == be32_to_cpu(d->dd_diskdq.d_id)) {
					xfs_trans_brelse(sc->tp, bp);
					continue;
				}
				error = -EFSCORRUPTED;
				xfs_trans_brelse(sc->tp, bp);
			}
			if (error != -EFSBADCRC && error != -EFSCORRUPTED)
				goto out;

			/* Failed verifier, try again. */
			error = xfs_trans_read_buf(sc->mp, sc->tp,
					sc->mp->m_ddev_targp,
					XFS_FSB_TO_DADDR(sc->mp, fsbno),
					qi->qi_dqchunklen,
					0, &bp, NULL);
			if (error)
				goto out;

			/*
			 * Fix the quota block, which will roll our transaction
			 * and release bp.
			 */
			error = xrep_quota_block(sc, bp, dqtype, id);
			if (error)
				goto out;
		}
	}

out:
	return error;
}

/*
 * Go fix anything in the quota items that we could have been mad about.  Now
 * that we've checked the quota inode data fork we have to drop ILOCK_EXCL to
 * use the regular dquot functions.
 */
STATIC int
xrep_quota_problems(
	struct xfs_scrub	*sc,
	uint			dqtype)
{
	struct xrep_quota_info	rqi;
	int			error;

	rqi.sc = sc;
	rqi.need_quotacheck = false;
	error = xfs_qm_dqiterate(sc->mp, dqtype, xrep_quota_item, &rqi);
	if (error)
		return error;

	/* Make a quotacheck happen. */
	if (rqi.need_quotacheck)
		xrep_force_quotacheck(sc, dqtype);
	return 0;
}

/* Repair all of a quota type's items. */
int
xrep_quota(
	struct xfs_scrub	*sc)
{
	uint			dqtype;
	int			error;

	dqtype = xchk_quota_to_dqtype(sc);

	/*
	 * Re-take the ILOCK so that we can fix any problems that we found
	 * with the data fork mappings, or with the dquot bufs themselves.
	 */
	if (sc->ilock_flags == 0) {
		sc->ilock_flags = XFS_ILOCK_EXCL;
		xfs_ilock(sc->ip, sc->ilock_flags);
	}
	error = xrep_quota_data_fork(sc, dqtype);
	if (error)
		goto out;

	/*
	 * Roll the transaction to unjoin the quota inode from transaction so
	 * that we can unlock the quota inode; we play only with dquots from
	 * now on.
	 */
	error = xfs_trans_roll(&sc->tp);
	if (error)
		goto out;
	xfs_iunlock(sc->ip, sc->ilock_flags);
	sc->ilock_flags = 0;

	/* Fix anything the dquot verifiers don't complain about. */
	error = xrep_quota_problems(sc, dqtype);
out:
	return error;
}
