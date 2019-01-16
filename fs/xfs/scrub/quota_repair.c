// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
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
#include "xfs_icache.h"
#include "xfs_inode_fork.h"
#include "xfs_alloc.h"
#include "xfs_bmap.h"
#include "xfs_bmap_util.h"
#include "xfs_ialloc.h"
#include "xfs_ialloc_btree.h"
#include "xfs_quota.h"
#include "xfs_qm.h"
#include "xfs_dquot.h"
#include "xfs_dquot_item.h"
#include "xfs_trans_space.h"
#include "xfs_error.h"
#include "xfs_errortag.h"
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
 *
 * Online quotacheck is fairly straightforward.  We engage a repair freeze,
 * zero all the dquots, and scan every inode in the system to recalculate the
 * appropriate quota charges.  Finally, we log all the dquots to disk and
 * set the _CHKD flags.
 */

struct xrep_quota_info {
	struct xfs_scrub	*sc;
	bool			need_quotacheck;
};

/* Scrub the fields in an individual quota item. */
STATIC int
xrep_quota_item(
	struct xfs_dquot	*dq,
	uint			dqtype,
	void			*priv)
{
	struct xrep_quota_info	*rqi = priv;
	struct xfs_scrub	*sc = rqi->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_disk_dquot	*d = &dq->q_core;
	unsigned long long	bsoft;
	unsigned long long	isoft;
	unsigned long long	rsoft;
	unsigned long long	bhard;
	unsigned long long	ihard;
	unsigned long long	rhard;
	unsigned long long	bcount;
	unsigned long long	icount;
	unsigned long long	rcount;
	xfs_ino_t		fs_icount;
	bool			dirty = false;
	int			error;

	/* Did we get the dquot type we wanted? */
	if (dqtype != (d->d_flags & XFS_DQ_ALLTYPES)) {
		d->d_flags = dqtype;
		dirty = true;
	}

	if (d->d_pad0 || d->d_pad) {
		d->d_pad0 = 0;
		d->d_pad = 0;
		dirty = true;
	}

	/* Check the limits. */
	bhard = be64_to_cpu(d->d_blk_hardlimit);
	ihard = be64_to_cpu(d->d_ino_hardlimit);
	rhard = be64_to_cpu(d->d_rtb_hardlimit);

	bsoft = be64_to_cpu(d->d_blk_softlimit);
	isoft = be64_to_cpu(d->d_ino_softlimit);
	rsoft = be64_to_cpu(d->d_rtb_softlimit);

	if (bsoft > bhard) {
		d->d_blk_softlimit = d->d_blk_hardlimit;
		dirty = true;
	}

	if (isoft > ihard) {
		d->d_ino_softlimit = d->d_ino_hardlimit;
		dirty = true;
	}

	if (rsoft > rhard) {
		d->d_rtb_softlimit = d->d_rtb_hardlimit;
		dirty = true;
	}

	/* Check the resource counts. */
	bcount = be64_to_cpu(d->d_bcount);
	icount = be64_to_cpu(d->d_icount);
	rcount = be64_to_cpu(d->d_rtbcount);
	fs_icount = percpu_counter_sum(&mp->m_icount);

	/*
	 * Check that usage doesn't exceed physical limits.  However, on
	 * a reflink filesystem we're allowed to exceed physical space
	 * if there are no quota limits.  We don't know what the real number
	 * is, but we can make quotacheck find out for us.
	 */
	if (!xfs_sb_version_hasreflink(&mp->m_sb) &&
	    mp->m_sb.sb_dblocks < bcount) {
		dq->q_res_bcount -= be64_to_cpu(dq->q_core.d_bcount);
		dq->q_res_bcount += mp->m_sb.sb_dblocks;
		d->d_bcount = cpu_to_be64(mp->m_sb.sb_dblocks);
		rqi->need_quotacheck = true;
		dirty = true;
	}
	if (icount > fs_icount) {
		dq->q_res_icount -= be64_to_cpu(dq->q_core.d_icount);
		dq->q_res_icount += fs_icount;
		d->d_icount = cpu_to_be64(fs_icount);
		rqi->need_quotacheck = true;
		dirty = true;
	}
	if (rcount > mp->m_sb.sb_rblocks) {
		dq->q_res_rtbcount -= be64_to_cpu(dq->q_core.d_rtbcount);
		dq->q_res_rtbcount += mp->m_sb.sb_rblocks;
		d->d_rtbcount = cpu_to_be64(mp->m_sb.sb_rblocks);
		rqi->need_quotacheck = true;
		dirty = true;
	}

	if (!dirty)
		return 0;

	dq->dq_flags |= XFS_DQ_DIRTY;
	xfs_trans_dqjoin(sc->tp, dq);
	xfs_trans_log_dquot(sc->tp, dq);
	error = xfs_trans_roll(&sc->tp);
	xfs_dqlock(dq);
	return error;
}

/* Fix a quota timer so that we can pass the verifier. */
STATIC void
xrep_quota_fix_timer(
	__be64			softlimit,
	__be64			countnow,
	__be32			*timer,
	time_t			timelimit)
{
	uint64_t		soft = be64_to_cpu(softlimit);
	uint64_t		count = be64_to_cpu(countnow);

	if (soft && count > soft && *timer == 0)
		*timer = cpu_to_be32(get_seconds() + timelimit);
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
				qi->qi_btimelimit);
		xrep_quota_fix_timer(ddq->d_ino_softlimit,
				ddq->d_icount, &ddq->d_itimer,
				qi->qi_itimelimit);
		xrep_quota_fix_timer(ddq->d_rtb_softlimit,
				ddq->d_rtbcount, &ddq->d_rtbtimer,
				qi->qi_rtbtimelimit);

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
	return xfs_trans_roll(&sc->tp);
}

/* Repair quota's data fork. */
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

/* Online Quotacheck */

/*
 * Add this inode's resource usage to the dquot.  We adjust the in-core and
 * the (cached) on-disk copies of the counters and leave the dquot dirty.  A
 * subsequent pass through the dquots logs them all to disk.  Fortunately we
 * froze the filesystem before starting so at least we don't have to deal
 * with chown/chproj races.
 */
STATIC int
xrep_quotacheck_dqadjust(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip,
	uint			type,
	xfs_qcnt_t		nblks,
	xfs_qcnt_t		rtblks)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_dquot	*dqp;
	xfs_dqid_t		id;
	int			error;

	/* Try to read in the dquot. */
	id = xfs_qm_id_for_quotatype(ip, type);
	error = xfs_qm_dqget(mp, id, type, false, &dqp);
	if (error == -ENOENT) {
		/* Allocate a dquot using our special transaction. */
		error = xfs_qm_dqget_alloc(&sc->tp, id, type, &dqp);
		if (error)
			return error;
		error = xfs_trans_roll_inode(&sc->tp, sc->ip);
	}
	if (error) {
		/*
		 * Shouldn't be able to turn off quotas here.
		 */
		ASSERT(error != -ESRCH);
		ASSERT(error != -ENOENT);
		return error;
	}

	/*
	 * Adjust the inode count and the block count to reflect this inode's
	 * resource usage.
	 */
	be64_add_cpu(&dqp->q_core.d_icount, 1);
	dqp->q_res_icount++;
	if (nblks) {
		be64_add_cpu(&dqp->q_core.d_bcount, nblks);
		dqp->q_res_bcount += nblks;
	}
	if (rtblks) {
		be64_add_cpu(&dqp->q_core.d_rtbcount, rtblks);
		dqp->q_res_rtbcount += rtblks;
	}

	/*
	 * Set default limits, adjust timers (since we changed usages)
	 *
	 * There are no timers for the default values set in the root dquot.
	 */
	if (dqp->q_core.d_id) {
		xfs_qm_adjust_dqlimits(mp, dqp);
		xfs_qm_adjust_dqtimers(mp, &dqp->q_core);
	}

	dqp->dq_flags |= XFS_DQ_DIRTY;
	xfs_qm_dqput(dqp);
	return 0;
}

/* Record this inode's quota use. */
STATIC int
xrep_quotacheck_inode(
	struct xfs_scrub	*sc,
	uint			dqtype,
	struct xfs_inode	*ip)
{
	struct xfs_ifork	*ifp;
	xfs_filblks_t		rtblks = 0;	/* total rt blks */
	xfs_qcnt_t		nblks;
	int			error;

	/* Count the realtime blocks. */
	if (XFS_IS_REALTIME_INODE(ip)) {
		ifp = XFS_IFORK_PTR(ip, XFS_DATA_FORK);

		if (!(ifp->if_flags & XFS_IFEXTENTS)) {
			error = xfs_iread_extents(sc->tp, ip, XFS_DATA_FORK);
			if (error)
				return error;
		}

		xfs_bmap_count_leaves(ifp, &rtblks);
	}

	nblks = (xfs_qcnt_t)ip->i_d.di_nblocks - rtblks;

	/* Adjust the dquot. */
	return xrep_quotacheck_dqadjust(sc, ip, dqtype, nblks, rtblks);
}

struct xrep_quotacheck {
	struct xfs_scrub	*sc;
	uint			dqtype;
};

/* Iterate all the inodes in an AG group. */
STATIC int
xrep_quotacheck_inobt(
	struct xfs_btree_cur		*cur,
	union xfs_btree_rec		*rec,
	void				*priv)
{
	struct xfs_inobt_rec_incore	irec;
	struct xfs_mount		*mp = cur->bc_mp;
	struct xfs_inode		*ip = NULL;
	struct xrep_quotacheck		*rq = priv;
	xfs_ino_t			ino;
	xfs_agino_t			agino;
	int				chunkidx;
	int				error = 0;

	xfs_inobt_btrec_to_irec(mp, rec, &irec);

	for (chunkidx = 0, agino = irec.ir_startino;
	     chunkidx < XFS_INODES_PER_CHUNK;
	     chunkidx++, agino++) {
		bool	inuse;

		/* Skip if this inode is free */
		if (XFS_INOBT_MASK(chunkidx) & irec.ir_free)
			continue;
		ino = XFS_AGINO_TO_INO(mp, cur->bc_private.a.agno, agino);
		if (xfs_is_quota_inode(&mp->m_sb, ino))
			continue;

		/* Back off and try again if an inode is being reclaimed */
		error = xfs_icache_inode_is_allocated(mp, NULL, ino, &inuse);
		if (error == -EAGAIN)
			return -EDEADLOCK;

		/*
		 * Grab inode for scanning.  We cannot use DONTCACHE here
		 * because we already have a transaction so the iput must not
		 * trigger inode reclaim (which might allocate a transaction
		 * to clean up posteof blocks).
		 */
		error = xfs_iget(mp, NULL, ino, 0, XFS_ILOCK_EXCL, &ip);
		if (error)
			return error;

		error = xrep_quotacheck_inode(rq->sc, rq->dqtype, ip);
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
		xfs_irele(ip);
		if (error)
			return error;
	}

	return 0;
}

/* Zero a dquot prior to regenerating the counts. */
static int
xrep_quotacheck_zero_dquot(
	struct xfs_dquot	*dq,
	uint			dqtype,
	void			*priv)
{
	dq->q_res_bcount -= be64_to_cpu(dq->q_core.d_bcount);
	dq->q_core.d_bcount = 0;
	dq->q_res_icount -= be64_to_cpu(dq->q_core.d_icount);
	dq->q_core.d_icount = 0;
	dq->q_res_rtbcount -= be64_to_cpu(dq->q_core.d_rtbcount);
	dq->q_core.d_rtbcount = 0;
	dq->dq_flags |= XFS_DQ_DIRTY;
	return 0;
}

/* Log a dirty dquot after we regenerated the counters. */
static int
xrep_quotacheck_log_dquot(
	struct xfs_dquot	*dq,
	uint			dqtype,
	void			*priv)
{
	struct xfs_scrub	*sc = priv;
	int			error;

	xfs_trans_dqjoin(sc->tp, dq);
	xfs_trans_log_dquot(sc->tp, dq);
	error = xfs_trans_roll(&sc->tp);
	xfs_dqlock(dq);
	return error;
}

/* Execute an online quotacheck. */
STATIC int
xrep_quotacheck(
	struct xfs_scrub	*sc,
	uint			dqtype)
{
	struct xrep_quotacheck	rq;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_buf		*bp;
	struct xfs_btree_cur	*cur;
	xfs_agnumber_t		ag;
	uint			flag;
	int			error;

	/*
	 * Commit the transaction so that we can allocate new quota ip
	 * mappings if we have to.  If we crash after this point, the sb
	 * still has the CHKD flags cleared, so mount quotacheck will fix
	 * all of this up.
	 */
	error = xfs_trans_commit(sc->tp);
	sc->tp = NULL;
	if (error)
		return error;

	/* Zero all the quota items. */
	error = xfs_qm_dqiterate(mp, dqtype, xrep_quotacheck_zero_dquot,
			sc);
	if (error)
		goto out;

	rq.sc = sc;
	rq.dqtype = dqtype;

	/* Iterate all AGs for inodes. */
	for (ag = 0; ag < mp->m_sb.sb_agcount; ag++) {
		error = xfs_ialloc_read_agi(mp, NULL, ag, &bp);
		if (error)
			goto out;
		cur = xfs_inobt_init_cursor(mp, NULL, bp, ag, XFS_BTNUM_INO);
		error = xfs_btree_query_all(cur, xrep_quotacheck_inobt, &rq);
		xfs_btree_del_cursor(cur, error);
		xfs_buf_relse(bp);
		if (error)
			goto out;
	}

	/* Log dquots. */
	error = xchk_trans_alloc(sc, 0);
	if (error)
		goto out;
	error = xfs_qm_dqiterate(mp, dqtype, xrep_quotacheck_log_dquot, sc);
	if (error)
		goto out;

	/* Set quotachecked flag. */
	flag = xfs_quota_chkd_flag(dqtype);
	sc->mp->m_qflags |= flag;
	spin_lock(&sc->mp->m_sb_lock);
	sc->mp->m_sb.sb_qflags |= flag;
	spin_unlock(&sc->mp->m_sb_lock);
	xfs_log_sb(sc->tp);
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
	if (rqi.need_quotacheck ||
	    XFS_TEST_ERROR(false, sc->mp, XFS_ERRTAG_FORCE_SCRUB_REPAIR))
		xrep_force_quotacheck(sc, dqtype);

	return 0;
}

/* Repair all of a quota type's items. */
int
xrep_quota(
	struct xfs_scrub	*sc)
{
	uint			dqtype;
	uint			flag;
	int			error;

	dqtype = xchk_quota_to_dqtype(sc);

	/* Fix problematic data fork mappings. */
	error = xrep_quota_data_fork(sc, dqtype);
	if (error)
		goto out;

	/* Unlock quota inode; we play only with dquots from now on. */
	xfs_iunlock(sc->ip, sc->ilock_flags);
	sc->ilock_flags = 0;

	/* Fix anything the dquot verifiers complain about. */
	error = xrep_quota_problems(sc, dqtype);
	if (error)
		goto out;

	/* Do we need a quotacheck?  Did we need one? */
	flag = xfs_quota_chkd_flag(dqtype);
	if (!(flag & sc->mp->m_qflags)) {
		/* We need to freeze the fs before we can scan inodes. */
		if (!(sc->flags & XCHK_FS_FROZEN)) {
			error = -EDEADLOCK;
			goto out;
		}

		error = xrep_quotacheck(sc, dqtype);
	}
out:
	return error;
}
