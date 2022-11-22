// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010 Red Hat, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_alloc.h"
#include "xfs_discard.h"
#include "xfs_error.h"
#include "xfs_extent_busy.h"
#include "xfs_trace.h"
#include "xfs_log.h"
#include "xfs_ag.h"
#include "xfs_health.h"
#include "xfs_rtbitmap.h"

/*
 * For trim functions that support it, cycle the metadata locks periodically
 * to prevent other parts of the filesystem from starving.
 */
#define XFS_TRIM_RELAX_INTERVAL	(HZ)

/* Trim the free space in this AG by block number. */
static inline int
xfs_trim_perag_bybno(
	struct xfs_perag	*pag,
	struct xfs_buf		**agbpp,
	xfs_daddr_t		start,
	xfs_daddr_t		end,
	xfs_daddr_t		minlen,
	uint64_t		*blocks_trimmed)
{
	struct xfs_mount	*mp = pag->pag_mount;
	struct block_device	*bdev = xfs_buftarg_bdev(mp->m_ddev_targp);
	struct xfs_btree_cur	*cur;
	struct xfs_agf		*agf = (*agbpp)->b_addr;
	xfs_agnumber_t		agno = pag->pag_agno;
	xfs_agblock_t		start_agbno;
	xfs_agblock_t		end_agbno;
	xfs_extlen_t		minlen_fsb = XFS_BB_TO_FSB(mp, minlen);
	unsigned long		last_relax = jiffies;
	int			i;
	int			error;

	start = max(start, XFS_AGB_TO_DADDR(mp, agno, 0));
	start_agbno = xfs_daddr_to_agbno(mp, start);

	end = min(end, XFS_AGB_TO_DADDR(mp, agno, be32_to_cpu(agf->agf_length)) - 1);
	end_agbno = xfs_daddr_to_agbno(mp, end);

	//trace_printk("%d agno 0x%x start_agbno 0x%x end_agbno 0x%x minlen_fsb 0x%x", __LINE__, agno, start_agbno, end_agbno, minlen_fsb);
	cur = xfs_allocbt_init_cursor(mp, NULL, *agbpp, pag, XFS_BTNUM_BNO);

	error = xfs_alloc_lookup_le(cur, start_agbno, 0, &i);
	if (error)
		goto out_del_cursor;

	/*
	 * If we didn't find anything at or below start_agbno, increment the
	 * cursor to see if there's another record above it.
	 */
	if (!i) {
		error = xfs_btree_increment(cur, 0, &i);
		if (error)
			goto out_del_cursor;
	}

	/* Loop the entire range that was asked for. */
	while (i) {
		xfs_agblock_t	fbno;
		xfs_extlen_t	flen;
		xfs_daddr_t	dbno;
		xfs_extlen_t	dlen;

		error = xfs_alloc_get_rec(cur, &fbno, &flen, &i);
		if (error)
			goto out_del_cursor;
		if (XFS_IS_CORRUPT(mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_del_cursor;
		}

		//trace_printk("%d agno 0x%x fbno 0x%x flen 0x%x", __LINE__, agno, fbno, flen);

		/* Skip extents entirely outside of the range. */
		if (fbno >= end_agbno)
			break;
		if (fbno + flen < start_agbno)
			goto next_extent;

		/* Trim the extent returned to the range we want. */
		{//unsigned int orig_fbno = fbno; unsigned int orig_flen = flen;
		if (fbno < start_agbno) {
			flen -= start_agbno - fbno;
			fbno = start_agbno;
		}
		if (fbno + flen > end_agbno + 1)
			flen = end_agbno - fbno + 1;
		//if (fbno != orig_fbno || flen != orig_flen)
		//	trace_printk("OOG agno 0x%x fbno 0x%x<-0x%x flen 0x%x<-0x%x", agno, fbno, orig_fbno, flen, orig_flen);
		}

		/* Ignore too small. */
		if (flen < minlen_fsb) {
			trace_xfs_discard_toosmall(mp, agno, fbno, flen);
			goto next_extent;
		}

		/*
		 * If any blocks in the range are still busy, skip the
		 * discard and try again the next time.
		 */
		if (xfs_extent_busy_search(mp, pag, fbno, flen)) {
			trace_xfs_discard_busy(mp, agno, fbno, flen);
			goto next_extent;
		}

		trace_xfs_discard_extent(mp, agno, fbno, flen);

		dbno = XFS_AGB_TO_DADDR(mp, agno, fbno);
		dlen = XFS_FSB_TO_BB(mp, flen);
		error = blkdev_issue_discard(bdev, dbno, dlen, GFP_NOFS);
		if (error)
			goto out_del_cursor;
		*blocks_trimmed += flen;

		if (time_after(jiffies, last_relax + XFS_TRIM_RELAX_INTERVAL)) {
			/*
			 * Cycle the AGF lock since we know how to pick up
			 * where we left off.
			 */
			trace_xfs_discard_relax(mp, agno, fbno, flen);
			xfs_btree_del_cursor(cur, error);
			xfs_buf_relse(*agbpp);

			error = xfs_alloc_read_agf(pag, NULL, 0, agbpp);
			if (error)
				return error;

			cur = xfs_allocbt_init_cursor(mp, NULL, *agbpp, pag,
					XFS_BTNUM_BNO);
			error = xfs_alloc_lookup_ge(cur, fbno + flen, 0, &i);
			last_relax = jiffies;
		} else {
next_extent:
			error = xfs_btree_increment(cur, 0, &i);
		}
		if (error)
			goto out_del_cursor;

		if (fatal_signal_pending(current)) {
			error = -ERESTARTSYS;
			goto out_del_cursor;
		}
	}

out_del_cursor:
	xfs_btree_del_cursor(cur, error);
	return error;
}

/* Trim the free space in this AG by length. */
static inline int
xfs_trim_perag_bylen(
	struct xfs_perag	*pag,
	struct xfs_buf		*agbp,
	xfs_daddr_t		minlen,
	uint64_t		*blocks_trimmed)
{
	struct xfs_mount	*mp = pag->pag_mount;
	struct block_device	*bdev = xfs_buftarg_bdev(mp->m_ddev_targp);
	struct xfs_btree_cur	*cur;
	struct xfs_agf		*agf = agbp->b_addr;
	xfs_agnumber_t		agno = pag->pag_agno;
	int			error;
	int			i;

	cur = xfs_allocbt_init_cursor(mp, NULL, agbp, pag, XFS_BTNUM_CNT);

	/*
	 * Look up the longest btree in the AGF and start with it.
	 */
	error = xfs_alloc_lookup_ge(cur, 0, be32_to_cpu(agf->agf_longest), &i);
	if (error)
		goto out_del_cursor;

	/*
	 * Loop until we are done with all extents that are large
	 * enough to be worth discarding.
	 */
	while (i) {
		xfs_agblock_t	fbno;
		xfs_extlen_t	flen;
		xfs_daddr_t	dbno;
		xfs_extlen_t	dlen;

		error = xfs_alloc_get_rec(cur, &fbno, &flen, &i);
		if (error)
			goto out_del_cursor;
		if (XFS_IS_CORRUPT(mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto out_del_cursor;
		}
		ASSERT(flen <= be32_to_cpu(agf->agf_longest));

		/*
		 * use daddr format for all range/len calculations as that is
		 * the format the range/len variables are supplied in by
		 * userspace.
		 */
		dbno = XFS_AGB_TO_DADDR(mp, agno, fbno);
		dlen = XFS_FSB_TO_BB(mp, flen);

		/*
		 * Too small?  Give up.
		 */
		if (dlen < minlen) {
			trace_xfs_discard_toosmall(mp, agno, fbno, flen);
			goto out_del_cursor;
		}

		/*
		 * If any blocks in the range are still busy, skip the
		 * discard and try again the next time.
		 */
		if (xfs_extent_busy_search(mp, pag, fbno, flen)) {
			trace_xfs_discard_busy(mp, agno, fbno, flen);
			goto next_extent;
		}

		trace_xfs_discard_extent(mp, agno, fbno, flen);
		error = blkdev_issue_discard(bdev, dbno, dlen, GFP_NOFS);
		if (error)
			goto out_del_cursor;
		*blocks_trimmed += flen;

next_extent:
		error = xfs_btree_decrement(cur, 0, &i);
		if (error)
			goto out_del_cursor;

		if (fatal_signal_pending(current)) {
			error = -ERESTARTSYS;
			goto out_del_cursor;
		}
	}

out_del_cursor:
	xfs_btree_del_cursor(cur, error);
	return error;
}

STATIC int
xfs_trim_perag(
	struct xfs_perag	*pag,
	xfs_daddr_t		start,
	xfs_daddr_t		end,
	xfs_daddr_t		minlen,
	uint64_t		*blocks_trimmed)
{
	struct xfs_mount	*mp = pag->pag_mount;
	struct xfs_buf		*agbp;
	struct xfs_agf		*agf;
	xfs_agnumber_t		agno = pag->pag_agno;
	int			error;

	/*
	 * Force out the log.  This means any transactions that might have freed
	 * space before we take the AGF buffer lock are now on disk, and the
	 * volatile disk cache is flushed.
	 */
	xfs_log_force(mp, XFS_LOG_SYNC);

	error = xfs_alloc_read_agf(pag, NULL, 0, &agbp);
	if (error)
		return error;
	agf = agbp->b_addr;

	//trace_printk("%d agno 0x%x start 0x%llx:0x%llx end 0x%llx:0x%llx minlen 0x%llx", __LINE__, pag->pag_agno, start, XFS_AGB_TO_DADDR(mp, agno, 0), end, XFS_AGB_TO_DADDR(mp, agno, be32_to_cpu(agf->agf_length)) - 1, minlen);

	if (start > XFS_AGB_TO_DADDR(mp, agno, 0) ||
	    end < XFS_AGB_TO_DADDR(mp, agno, be32_to_cpu(agf->agf_length)) - 1) {
		/* Only trimming part of this AG */
		error = xfs_trim_perag_bybno(pag, &agbp, start, end, minlen,
				blocks_trimmed);
	} else {
		/* Trim this entire AG */
		error = xfs_trim_perag_bylen(pag, agbp, minlen,
				blocks_trimmed);
	}

	xfs_buf_relse(agbp);
	return error;
}

static int
xfs_trim_ddev_extents(
	struct xfs_mount	*mp,
	xfs_daddr_t		start,
	xfs_daddr_t		end,
	xfs_daddr_t		minlen,
	uint64_t		*blocks_trimmed)
{
	struct xfs_perag	*pag;
	xfs_agnumber_t		start_agno, end_agno;
	int			error, last_error = 0;

	if (end > XFS_FSB_TO_BB(mp, mp->m_sb.sb_dblocks) - 1)
		end = XFS_FSB_TO_BB(mp, mp->m_sb.sb_dblocks) - 1;

	start_agno = xfs_daddr_to_agno(mp, start);
	end_agno = xfs_daddr_to_agno(mp, end);

	for_each_perag_range(mp, start_agno, end_agno, pag) {
		error = xfs_trim_perag(pag, start, end, minlen,
				blocks_trimmed);
		if (error == -ERESTARTSYS) {
			xfs_perag_put(pag);
			return error;
		}
		if (error)
			last_error = error;
	}

	return last_error;
}

#ifdef CONFIG_XFS_RT
struct xfs_trim_rtdev {
	uint64_t		*blocks_trimmed;
	xfs_rtblock_t		minlen_fsb;
	unsigned long		last_relax;
};

static int
xfs_trim_rtdev_extent(
	struct xfs_mount		*mp,
	struct xfs_trans		*tp,
	const struct xfs_rtalloc_rec	*rec,
	void				*priv)
{
	struct block_device	*bdev = xfs_buftarg_bdev(mp->m_rtdev_targp);
	struct xfs_trim_rtdev	*tr = priv;
	xfs_rtblock_t		rbno, rlen;
	xfs_daddr_t		dbno, dlen;
	int			error;

	if (fatal_signal_pending(current))
		return -ERESTARTSYS;

	rbno = xfs_rtx_to_rtb(mp, rec->ar_startext);
	rlen = xfs_rtx_to_rtb(mp, rec->ar_extcount);

	/* Ignore too small. */
	if (rlen < tr->minlen_fsb) {
		trace_xfs_discard_rttoosmall(mp, rbno, rlen);
		goto out;
	}

	trace_xfs_discard_rtextent(mp, rbno, rlen);

	dbno = XFS_FSB_TO_BB(mp, rbno);
	dlen = XFS_FSB_TO_BB(mp, rlen);

	error = blkdev_issue_discard(bdev, dbno, dlen, GFP_NOFS);
	if (error)
		return error;

	*tr->blocks_trimmed += rlen;

out:
	if (time_after(jiffies, tr->last_relax + XFS_TRIM_RELAX_INTERVAL)) {
		/*
		 * Cycle the rtbitmap lock since we know how to pick up
		 * where we left off.
		 */
		trace_xfs_discard_rtrelax(mp, rbno, rlen);
		xfs_rtbitmap_unlock_shared(mp, XFS_RBMLOCK_BITMAP);
		xfs_rtbitmap_lock_shared(mp, XFS_RBMLOCK_BITMAP);
		tr->last_relax = jiffies;
	}

	return 0;
}

static int
xfs_trim_rtdev_extents(
	struct xfs_mount	*mp,
	xfs_daddr_t		start,
	xfs_daddr_t		end,
	xfs_daddr_t		minlen,
	uint64_t		*blocks_trimmed)
{
	struct xfs_rtalloc_rec	low = { }, high = { };
	struct xfs_trim_rtdev	tr = {
		.blocks_trimmed	= blocks_trimmed,
		.minlen_fsb	= XFS_BB_TO_FSB(mp, minlen),
		.last_relax	= jiffies,
	};
	xfs_daddr_t		rtdev_daddr;
	xfs_extlen_t		mod;
	int			error;

	/* Shift the start and end downwards to match the rt device. */
	rtdev_daddr = XFS_FSB_TO_BB(mp, mp->m_sb.sb_dblocks);
	if (start > rtdev_daddr)
		start -= rtdev_daddr;
	else
		start = 0;

	if (end <= rtdev_daddr)
		return 0;
	end -= rtdev_daddr;

	if (end > XFS_FSB_TO_BB(mp, mp->m_sb.sb_rblocks) - 1)
		end = XFS_FSB_TO_BB(mp, mp->m_sb.sb_rblocks) - 1;

	/* Convert the rt blocks to rt extents */
	low.ar_startext = xfs_rtb_to_rtx(mp, XFS_BB_TO_FSB(mp, start), &mod);
	if (mod)
		low.ar_startext++;
	high.ar_startext = xfs_rtb_to_rtx(mp, XFS_BB_TO_FSBT(mp, end), &mod);

	/*
	 * Walk the free ranges between low and high.  The query_range function
	 * trims the extents returned.
	 */
	xfs_rtbitmap_lock_shared(mp, XFS_RBMLOCK_BITMAP);
	error = xfs_rtalloc_query_range(mp, NULL, &low, &high,
			xfs_trim_rtdev_extent, &tr);
	xfs_rtbitmap_unlock_shared(mp, XFS_RBMLOCK_BITMAP);
	return error;
}
#else
# define xfs_trim_rtdev_extents(m,s,e,n,b)	(-EOPNOTSUPP)
#endif /* CONFIG_XFS_RT */

/*
 * trim a range of the filesystem.
 *
 * Note: the parameters passed from userspace are byte ranges into the
 * filesystem which does not match to the format we use for filesystem block
 * addressing. FSB addressing is sparse (AGNO|AGBNO), while the incoming format
 * is a linear address range. Hence we need to use DADDR based conversions and
 * comparisons for determining the correct offset and regions to trim.
 *
 * The realtime device is mapped into the FITRIM "address space" immediately
 * after the data device.
 */
int
xfs_ioc_trim(
	struct xfs_mount		*mp,
	struct fstrim_range __user	*urange)
{
	struct block_device	*bdev = xfs_buftarg_bdev(mp->m_ddev_targp);
	struct block_device	*rt_bdev = NULL;
	unsigned int		granularity = bdev_discard_granularity(bdev);
	struct fstrim_range	range;
	xfs_rfsblock_t		max_blocks;
	xfs_daddr_t		start, end, minlen;
	uint64_t		blocks_trimmed = 0;
	int			error, last_error = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!bdev_max_discard_sectors(bdev))
		return -EOPNOTSUPP;

	if (mp->m_rtdev_targp) {
		rt_bdev = xfs_buftarg_bdev(mp->m_rtdev_targp);
		if (!bdev_max_discard_sectors(rt_bdev))
			return -EOPNOTSUPP;
		granularity = max(granularity,
				  bdev_discard_granularity(rt_bdev));
	}

	/*
	 * We haven't recovered the log, so we cannot use our bnobt-guided
	 * storage zapping commands.
	 */
	if (xfs_has_norecovery(mp))
		return -EROFS;

	if (copy_from_user(&range, urange, sizeof(range)))
		return -EFAULT;

	range.minlen = max_t(u64, granularity, range.minlen);
	minlen = BTOBB(range.minlen);
	/*
	 * Truncating down the len isn't actually quite correct, but using
	 * BBTOB would mean we trivially get overflows for values
	 * of ULLONG_MAX or slightly lower.  And ULLONG_MAX is the default
	 * used by the fstrim application.  In the end it really doesn't
	 * matter as trimming blocks is an advisory interface.
	 */
	max_blocks = mp->m_sb.sb_dblocks + mp->m_sb.sb_rblocks;
	if (range.start >= XFS_FSB_TO_B(mp, max_blocks) ||
	    range.minlen > XFS_FSB_TO_B(mp, mp->m_ag_max_usable) ||
	    range.len < mp->m_sb.sb_blocksize)
		return -EINVAL;

	start = BTOBB(range.start);
	end = start + BTOBBT(range.len) - 1;

	error = xfs_trim_ddev_extents(mp, start, end, minlen, &blocks_trimmed);
	if (error == -ERESTARTSYS)
		return error;
	if (error)
		last_error = error;

	if (rt_bdev) {
		error = xfs_trim_rtdev_extents(mp, start, end, minlen,
				&blocks_trimmed);
		if (error == -ERESTARTSYS)
			return error;
		if (error)
			last_error = error;
	}

	if (last_error)
		return last_error;

	range.len = XFS_FSB_TO_B(mp, blocks_trimmed);
	if (copy_to_user(urange, &range, sizeof(range)))
		return -EFAULT;
	return 0;
}
