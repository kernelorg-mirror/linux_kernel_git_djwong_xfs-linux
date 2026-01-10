// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_bit.h"
#include "xfs_btree.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_trans.h"
#include "xfs_alloc.h"
#include "xfs_ag.h"
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_rtgroup.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_healthmon.h"
#include "xfs_trace.h"
#include "xfs_verify_media.h"

#include <linux/fserror.h>

struct xfs_group_data_lost {
	xfs_agblock_t		startblock;
	xfs_extlen_t		blockcount;
};

/* Report lost file data from rmap records */
static int
xfs_report_one_data_lost(
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec,
	void				*data)
{
	struct xfs_mount		*mp = cur->bc_mp;
	struct xfs_inode		*ip;
	struct xfs_group_data_lost	*lost = data;
	xfs_fileoff_t			fileoff = rec->rm_offset;
	xfs_extlen_t			blocks = rec->rm_blockcount;
	const xfs_agblock_t		lost_end =
			lost->startblock + lost->blockcount;
	const xfs_agblock_t		rmap_end =
			rec->rm_startblock + rec->rm_blockcount;
	int				error = 0;

	if (XFS_RMAP_NON_INODE_OWNER(rec->rm_owner) ||
	    (rec->rm_flags & (XFS_RMAP_ATTR_FORK | XFS_RMAP_BMBT_BLOCK)))
		return 0;

	error = xfs_iget(mp, cur->bc_tp, rec->rm_owner, 0, 0, &ip);
	if (error)
		return 0;

	if (lost->startblock > rec->rm_startblock) {
		fileoff += lost->startblock - rec->rm_startblock;
		blocks -= lost->startblock - rec->rm_startblock;
	}
	if (rmap_end > lost_end)
		blocks -= rmap_end - lost_end;

	fserror_report_data_lost(VFS_I(ip), XFS_FSB_TO_B(mp, fileoff),
			XFS_FSB_TO_B(mp, blocks), GFP_NOFS);

	xfs_irele(ip);
	return 0;
}

/* Walk reverse mappings to look for file data */
static int
xfs_report_data_lost(
	struct xfs_mount	*mp,
	enum xfs_group_type	type,
	xfs_daddr_t		daddr,
	u64			bblen)
{
	struct xfs_group	*xg = NULL;
	struct xfs_trans	*tp;
	xfs_fsblock_t		start_bno, end_bno;
	uint32_t		start_gno, end_gno;
	int			error;

	if (type == XG_TYPE_RTG) {
		start_bno = xfs_daddr_to_rtb(mp, daddr);
		end_bno = xfs_daddr_to_rtb(mp, daddr + bblen - 1);
	} else {
		start_bno = XFS_DADDR_TO_FSB(mp, daddr);
		end_bno = XFS_DADDR_TO_FSB(mp, daddr + bblen - 1);
	}

	tp = xfs_trans_alloc_empty(mp);
	start_gno = xfs_fsb_to_gno(mp, start_bno, type);
	end_gno = xfs_fsb_to_gno(mp, end_bno, type);
	while ((xg = xfs_group_next_range(mp, xg, start_gno, end_gno, type))) {
		struct xfs_buf		*agf_bp = NULL;
		struct xfs_rtgroup	*rtg = NULL;
		struct xfs_btree_cur	*cur;
		struct xfs_rmap_irec	ri_low = { };
		struct xfs_rmap_irec	ri_high;
		struct xfs_group_data_lost lost;

		if (type == XG_TYPE_AG) {
			struct xfs_perag	*pag = to_perag(xg);

			error = xfs_alloc_read_agf(pag, tp, 0, &agf_bp);
			if (error) {
				xfs_perag_put(pag);
				break;
			}

			cur = xfs_rmapbt_init_cursor(mp, tp, agf_bp, pag);
		} else {
			rtg = to_rtg(xg);
			xfs_rtgroup_lock(rtg, XFS_RTGLOCK_RMAP);
			cur = xfs_rtrmapbt_init_cursor(tp, rtg);
		}

		/*
		 * Set the rmap range from ri_low to ri_high, which represents
		 * a [start, end] where we looking for the files or metadata.
		 */
		memset(&ri_high, 0xFF, sizeof(ri_high));
		if (xg->xg_gno == start_gno)
			ri_low.rm_startblock =
				xfs_fsb_to_gbno(mp, start_bno, type);
		if (xg->xg_gno == end_gno)
			ri_high.rm_startblock =
				xfs_fsb_to_gbno(mp, end_bno, type);

		lost.startblock = ri_low.rm_startblock;
		lost.blockcount = min(xg->xg_block_count,
				      ri_high.rm_startblock + 1) -
							ri_low.rm_startblock;

		error = xfs_rmap_query_range(cur, &ri_low, &ri_high,
				xfs_report_one_data_lost, &lost);
		xfs_btree_del_cursor(cur, error);
		if (agf_bp)
			xfs_trans_brelse(tp, agf_bp);
		if (rtg)
			xfs_rtgroup_unlock(rtg, XFS_RTGLOCK_RMAP);
		if (error) {
			xfs_group_put(xg);
			break;
		}
	}

	xfs_trans_cancel(tp);
	return 0;
}

/* Allocate as much memory as we can get for verification buffer. */
static inline struct folio *
xfs_verify_alloc_folio(
	const unsigned int	iosize)
{
	unsigned int		order = get_order(iosize);

	while (order > 0) {
		struct folio	*folio =
			folio_alloc(GFP_KERNEL | __GFP_NORETRY, order);

		if (folio)
			return folio;
		order--;
	}

	return folio_alloc(GFP_KERNEL, 0);
}

/* Compute size of verification IO */
static unsigned int
xfs_verify_io_bbcount(
	uint64_t		bbcount,
	unsigned int		iosize,
	unsigned int		foliosize)
{
	/*
	 * I/O size is the minimum of the remaining range, the desired I/O
	 * size, and the amount of data we can attach to this bio given the
	 * folio size.  We never allocate a folio larger than 1MB so the
	 * multiplication cannot overflow.
	 */
	return min3(bbcount,
		    iosize >> SECTOR_SHIFT,
		    BIO_MAX_VECS * foliosize >> SECTOR_SHIFT);
}

/* Construct a bio for doing the verification. */
static struct bio *
xfs_verify_bio_alloc(
	struct xfs_buftarg	*btp,
	xfs_daddr_t		daddr,
	uint64_t		bbcount,
	struct folio		*folio,
	unsigned int		iosize,
	unsigned int		*bio_bbcount)
{
	struct bio		*bio;
	const unsigned int	foliosize = folio_size(folio);
	unsigned int		io_bbcount =
		xfs_verify_io_bbcount(bbcount, iosize, foliosize);
	const unsigned int	nr_vecs =
		howmany(io_bbcount << SECTOR_SHIFT, foliosize);
	unsigned int		i;

	bio = bio_alloc(btp->bt_bdev, nr_vecs, REQ_OP_READ, GFP_KERNEL);
	if (!bio)
		return NULL;

	bio->bi_iter.bi_sector = daddr;

	/*
	 * In the happy case, foliosize == iosize, and we only have to add the
	 * single large folio to the biovec.  However, if foliosize < iosize,
	 * try to add the same folio repeatedly so that the device can work on
	 * one large IO, even if doing multiple DMA transfers (all to the same
	 * physical memory) is less efficient.  There's still less overhead
	 * than issuing a large number of small bios.
	 */
	for (i = 0; i < nr_vecs; i++) {
		const unsigned int	vec_bbcount =
			min(io_bbcount, foliosize >> SECTOR_SHIFT);

		bio_add_folio_nofail(bio, folio, vec_bbcount << SECTOR_SHIFT,
				0);

		io_bbcount -= vec_bbcount;
		*bio_bbcount += vec_bbcount;
	}

	return bio;
}

/* Report a media error */
static void
xfs_verify_media_error(
	struct xfs_mount	*mp,
	struct xfs_verify_media	*me,
	struct xfs_buftarg	*btp,
	xfs_daddr_t		daddr,
	unsigned int		bio_bbcount,
	int			error)
{
	trace_xfs_verify_media_error(mp, me, btp->bt_bdev->bd_dev, daddr,
			bio_bbcount, error);

	/*
	 * Pass the I/O error up to the caller if we didn't successfully verify
	 * any bytes at all.
	 */
	if (me->me_start_daddr == daddr)
		me->me_ioerror = -error;

	if (!(me->me_flags & XFS_VERIFY_MEDIA_REPORT))
		return;

	xfs_healthmon_report_media(mp, me->me_dev, daddr, bio_bbcount);

	if (!xfs_has_rmapbt(mp))
		return;

	switch (me->me_dev) {
	case XFS_DEV_DATA:
		xfs_report_data_lost(mp, XG_TYPE_AG, daddr, bio_bbcount);
		break;
	case XFS_DEV_RT:
		xfs_report_data_lost(mp, XG_TYPE_RTG, daddr, bio_bbcount);
		break;
	}
}

/* Verify the media of an xfs device by submitting read requests to the disk. */
static int
xfs_verify_media(
	struct xfs_mount	*mp,
	struct xfs_verify_media	*me)
{
	struct xfs_buftarg	*btp = NULL;
	struct folio		*folio;
	xfs_daddr_t		daddr;
	uint64_t		bbcount;
	unsigned int		iosize;
	int			error = 0;

	me->me_ioerror = 0;

	switch (me->me_dev) {
	case XFS_DEV_DATA:
		btp = mp->m_ddev_targp;
		break;
	case XFS_DEV_LOG:
		if (mp->m_logdev_targp->bt_bdev != mp->m_ddev_targp->bt_bdev)
			btp = mp->m_logdev_targp;
		break;
	case XFS_DEV_RT:
		btp = mp->m_rtdev_targp;
		break;
	}
	if (!btp)
		return -ENODEV;

	/*
	 * If the caller told us to verify beyond the end of the disk, tell the
	 * user exactly where that was.
	 */
	if (me->me_end_daddr > btp->bt_nr_sectors)
		me->me_end_daddr = btp->bt_nr_sectors;

	/*
	 * end_daddr is the exclusive end of the range, so if start_daddr
	 * reaches there (or beyond), there's no work to be done.
	 */
	if (me->me_start_daddr >= me->me_end_daddr)
		return 0;

	/*
	 * To minimize command overhead, we'd like to create bios that are the
	 * maximal IO size supported by this device and allowed by the user.
	 * We'll try to allocate a single large folio for the entire IO, but we
	 * can fall back to a single base page mapped repeatedly by the biovec.
	 */
	iosize = min(queue_max_bytes(bdev_get_queue(btp->bt_bdev)), SZ_1M);
	if (me->me_max_io_size && iosize > me->me_max_io_size)
		iosize = me->me_max_io_size;
	if (iosize < SECTOR_SIZE)
		iosize = SECTOR_SIZE;

	daddr = me->me_start_daddr;
	bbcount = min_t(sector_t, me->me_end_daddr, btp->bt_nr_sectors) -
			  me->me_start_daddr;

	/*
	 * There are three ranges involved here:
	 *
	 *  - [me->me_start_daddr, me->me_end_daddr) is the range that the
	 *    user wants to verify.  end_daddr can be beyond the end of the
	 *    disk; we'll constrain it to the end if necessary.
	 *
	 *  - [daddr, me->me_end_daddr) is the range that we have not yet
	 *    verified.  We update daddr after each successful read.
	 *    me->me_start_daddr is set to daddr before returning.
	 *
	 *  - [daddr, daddr + bio_bbcount) is the range that we're currently
	 *    verifying.
	 */
	if ((iosize >> SECTOR_SHIFT) > bbcount)
		iosize = bbcount << SECTOR_SHIFT;

	folio = xfs_verify_alloc_folio(iosize);
	if (!folio)
		return -ENOMEM;

	trace_xfs_verify_media(mp, me, btp->bt_bdev->bd_dev, daddr,
			bbcount, iosize, folio_size(folio));

	while (bbcount > 0) {
		struct bio		*bio;
		unsigned int		bio_bbcount = 0;

		bio = xfs_verify_bio_alloc(btp, daddr, bbcount, folio, iosize,
				&bio_bbcount);
		if (!bio) {
			error = -ENOMEM;
			break;
		}

		error = submit_bio_wait(bio);
		bio_put(bio);
		if (error) {
			xfs_verify_media_error(mp, me, btp, daddr, bio_bbcount,
					error);
			error = 0;
			break;
		}

		daddr += bio_bbcount;
		bbcount -= bio_bbcount;

		if (bbcount == 0)
			break;

		if (me->me_rest_us) {
			ktime_t		expires;

			expires = ktime_add_ns(ktime_get(),
					me->me_rest_us * 1000);
			set_current_state(TASK_KILLABLE);
			schedule_hrtimeout(&expires, HRTIMER_MODE_ABS);
		}

		if (fatal_signal_pending(current)) {
			error = -EINTR;
			break;
		}

		cond_resched();
	}

	folio_put(folio);

	if (error)
		return error;

	/*
	 * Advance start_daddr to the end of what we verified if there wasn't
	 * an operational error.
	 */
	me->me_start_daddr = daddr;
	trace_xfs_verify_media_end(mp, me, btp->bt_bdev->bd_dev);
	return 0;
}

int
xfs_ioc_verify_media(
	struct file			*file,
	struct xfs_verify_media __user	*arg)
{
	struct xfs_verify_media		me;
	struct xfs_inode		*ip = XFS_I(file_inode(file));
	struct xfs_mount		*mp = ip->i_mount;
	int				error;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (copy_from_user(&me, arg, sizeof(me)))
		return -EFAULT;

	if (me.me_pad)
		return -EINVAL;
	if (me.me_flags & ~XFS_VERIFY_MEDIA_FLAGS)
		return -EINVAL;

	switch (me.me_dev) {
	case XFS_DEV_DATA:
	case XFS_DEV_LOG:
	case XFS_DEV_RT:
		break;
	default:
		return -EINVAL;
	}

	error = xfs_verify_media(mp, &me);
	if (error)
		return error;

	if (copy_to_user(arg, &me, sizeof(me)))
		return -EFAULT;

	return 0;
}
