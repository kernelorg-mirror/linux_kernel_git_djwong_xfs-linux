// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_btree.h"
#include "xfs_trace.h"
#include "xfs_alloc.h"
#include "xfs_bit.h"
#include "xfs_fsrefs.h"
#include "xfs_refcount.h"
#include "xfs_refcount_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_rtalloc.h"
#include "xfs_rtrefcount_btree.h"
#include "xfs_ag.h"
#include "xfs_rtbitmap.h"
#include "xfs_rtgroup.h"

/* getfsrefs query state */
struct xfs_fsrefs_info {
	struct xfs_fsrefs_head	*head;
	struct xfs_getfsrefs	*fsrefs_recs;	/* mapping records */

	struct xfs_btree_cur	*refc_cur;	/* refcount btree cursor */
	struct xfs_btree_cur	*bno_cur;	/* bnobt btree cursor */

	struct xfs_buf		*agf_bp;	/* AGF, for refcount queries */
	struct xfs_perag	*pag;		/* perag structure */
	struct xfs_rtgroup	*rtg;

	xfs_daddr_t		next_daddr;	/* next daddr we expect */
	/* daddr of low fsmap key when we're using the rtbitmap */
	xfs_daddr_t		low_daddr;

	/*
	 * Low refcount key for the query.  If low.rc_blockcount is nonzero,
	 * this is the second (or later) call to retrieve the recordset in
	 * pieces.  xfs_getfsrefs_rec_before_start will compare all records
	 * retrieved by the refcountbt query to filter out any records that
	 * start before the last record.
	 */
	struct xfs_refcount_irec low;
	struct xfs_refcount_irec high;		/* high refcount key */

	u32			dev;		/* device id */
	bool			last;		/* last extent? */
};

/* Associate a device with a getfsrefs handler. */
struct xfs_fsrefs_dev {
	u32			dev;
	int			(*fn)(struct xfs_trans *tp,
				      const struct xfs_fsrefs *keys,
				      struct xfs_fsrefs_info *info);
};

/* Convert an xfs_fsrefs to an fsrefs. */
static void
xfs_fsrefs_from_internal(
	struct xfs_getfsrefs	*dest,
	struct xfs_fsrefs	*src)
{
	dest->fcr_device = src->fcr_device;
	dest->fcr_flags = src->fcr_flags;
	dest->fcr_physical = BBTOB(src->fcr_physical);
	dest->fcr_owners = src->fcr_owners;
	dest->fcr_length = BBTOB(src->fcr_length);
	dest->fcr_reserved[0] = 0;
	dest->fcr_reserved[1] = 0;
	dest->fcr_reserved[2] = 0;
	dest->fcr_reserved[3] = 0;
}

/* Convert an fsrefs to an xfs_fsrefs. */
void
xfs_fsrefs_to_internal(
	struct xfs_fsrefs	*dest,
	struct xfs_getfsrefs	*src)
{
	dest->fcr_device = src->fcr_device;
	dest->fcr_flags = src->fcr_flags;
	dest->fcr_physical = BTOBBT(src->fcr_physical);
	dest->fcr_owners = src->fcr_owners;
	dest->fcr_length = BTOBBT(src->fcr_length);
}

/* Compare two getfsrefs device handlers. */
static int
xfs_fsrefs_dev_compare(
	const void			*p1,
	const void			*p2)
{
	const struct xfs_fsrefs_dev	*d1 = p1;
	const struct xfs_fsrefs_dev	*d2 = p2;

	return d1->dev - d2->dev;
}

static inline bool
xfs_fsrefs_rec_before_start(
	struct xfs_fsrefs_info		*info,
	const struct xfs_refcount_irec	*rec,
	xfs_daddr_t			rec_daddr)
{
	if (info->low_daddr != -1ULL)
		return rec_daddr < info->low_daddr;
	if (info->low.rc_blockcount)
		return rec->rc_startblock < info->low.rc_startblock;
	return false;
}

/*
 * Format a refcount record for fsrefs, having translated rc_startblock into
 * the appropriate daddr units.
 */
STATIC int
xfs_fsrefs_helper(
	struct xfs_trans		*tp,
	struct xfs_fsrefs_info		*info,
	const struct xfs_refcount_irec	*rec,
	xfs_daddr_t			rec_daddr,
	xfs_daddr_t			len_daddr)
{
	struct xfs_fsrefs		fcr;
	struct xfs_getfsrefs		*row;
	struct xfs_mount		*mp = tp->t_mountp;

	if (fatal_signal_pending(current))
		return -EINTR;

	if (len_daddr == 0)
		len_daddr = XFS_FSB_TO_BB(mp, rec->rc_blockcount);

	/*
	 * Filter out records that start before our startpoint, if the
	 * caller requested that.
	 */
	if (xfs_fsrefs_rec_before_start(info, rec, rec_daddr))
		return 0;

	/* Are we just counting mappings? */
	if (info->head->fch_count == 0) {
		if (info->head->fch_entries == UINT_MAX)
			return -ECANCELED;

		info->head->fch_entries++;
		return 0;
	}

	/* Fill out the extent we found */
	if (info->head->fch_entries >= info->head->fch_count)
		return -ECANCELED;

	if (info->pag)
		trace_xfs_fsrefs_mapping(mp, info->dev, info->pag->pag_agno,
				rec);
	else if (info->rtg)
		trace_xfs_fsrefs_mapping(mp, info->dev, info->rtg->rtg_rgno,
				rec);
	else
		trace_xfs_fsrefs_mapping(mp, info->dev, NULLAGNUMBER, rec);

	fcr.fcr_device = info->dev;
	fcr.fcr_flags = 0;
	fcr.fcr_physical = rec_daddr;
	fcr.fcr_owners = rec->rc_refcount;
	fcr.fcr_length = len_daddr;

	trace_xfs_getfsrefs_mapping(mp, &fcr);

	row = &info->fsrefs_recs[info->head->fch_entries++];
	xfs_fsrefs_from_internal(row, &fcr);
	return 0;
}

/* Synthesize fsrefs records from free space data. */
STATIC int
xfs_fsrefs_ddev_bnobt_helper(
	struct xfs_btree_cur		*cur,
	const struct xfs_alloc_rec_incore *rec,
	void				*priv)
{
	struct xfs_refcount_irec	irec;
	struct xfs_mount		*mp = cur->bc_mp;
	struct xfs_fsrefs_info		*info = priv;
	xfs_agnumber_t			next_agno;
	xfs_agblock_t			next_agbno;
	xfs_daddr_t			rec_daddr;

	/*
	 * Figure out if there's a gap between the last fsrefs record we
	 * emitted and this free extent.  If there is, report the gap as a
	 * refcount==1 record.
	 */
	next_agno = xfs_daddr_to_agno(mp, info->next_daddr);
	next_agbno = xfs_daddr_to_agbno(mp, info->next_daddr);

	ASSERT(next_agno >= cur->bc_ag.pag->pag_agno);
	ASSERT(rec->ar_startblock >= next_agbno);

	/*
	 * If we've already moved on to the next AG, we don't have any fsrefs
	 * records to synthesize.
	 */
	if (next_agno > cur->bc_ag.pag->pag_agno)
		return 0;

	info->next_daddr = XFS_AGB_TO_DADDR(mp, cur->bc_ag.pag->pag_agno,
			rec->ar_startblock + rec->ar_blockcount);

	if (rec->ar_startblock == next_agbno)
		return 0;

	/* Emit a record for the in-use space */
	irec.rc_startblock = next_agbno;
	irec.rc_blockcount = rec->ar_startblock - next_agbno;
	irec.rc_refcount = 1;
	irec.rc_domain = XFS_REFC_DOMAIN_SHARED;
	rec_daddr = XFS_AGB_TO_DADDR(mp, cur->bc_ag.pag->pag_agno,
			irec.rc_startblock);

	return xfs_fsrefs_helper(cur->bc_tp, info, &irec, rec_daddr, 0);
}

/* Emit records to fill a gap in the refcount btree with singly-owned blocks. */
STATIC int
xfs_fsrefs_ddev_fill_refcount_gap(
	struct xfs_trans		*tp,
	struct xfs_fsrefs_info		*info,
	xfs_agblock_t			agbno)
{
	struct xfs_alloc_rec_incore	low = {0};
	struct xfs_alloc_rec_incore	high = {0};
	struct xfs_mount		*mp = tp->t_mountp;
	struct xfs_btree_cur		*cur = info->bno_cur;
	struct xfs_agf			*agf;
	int				error;

	ASSERT(xfs_daddr_to_agno(mp, info->next_daddr) ==
			cur->bc_ag.pag->pag_agno);

	low.ar_startblock = xfs_daddr_to_agbno(mp, info->next_daddr);
	if (low.ar_startblock >= agbno)
		return 0;

	high.ar_startblock = agbno;
	error = xfs_alloc_query_range(cur, &low, &high,
			xfs_fsrefs_ddev_bnobt_helper, info);
	if (error)
		return error;

	/*
	 * Synthesize records for single-owner extents between the last
	 * fsrefcount record emitted and the end of the query range.
	 */
	agf = cur->bc_ag.agbp->b_addr;
	low.ar_startblock = min_t(xfs_agblock_t, agbno,
				  be32_to_cpu(agf->agf_length));
	if (xfs_daddr_to_agbno(mp, info->next_daddr) > low.ar_startblock)
		return 0;

	info->last = true;
	return xfs_fsrefs_ddev_bnobt_helper(cur, &low, info);
}

/* Transform a refcountbt irec into a fsrefs */
STATIC int
xfs_fsrefs_ddev_refcountbt_helper(
	struct xfs_btree_cur		*cur,
	const struct xfs_refcount_irec	*rec,
	void				*priv)
{
	struct xfs_mount		*mp = cur->bc_mp;
	struct xfs_fsrefs_info		*info = priv;
	xfs_daddr_t			rec_daddr;
	int				error;

	/*
	 * Stop once we get to the CoW staging extents; they're all shoved to
	 * the right side of the btree and were already covered by the bnobt
	 * scan.
	 */
	if (rec->rc_domain != XFS_REFC_DOMAIN_SHARED)
		return -ECANCELED;

	/* Report on any gaps first */
	error = xfs_fsrefs_ddev_fill_refcount_gap(cur->bc_tp, info,
			rec->rc_startblock);
	if (error)
		return error;

	rec_daddr = XFS_AGB_TO_DADDR(mp, cur->bc_ag.pag->pag_agno,
			rec->rc_startblock);
	info->next_daddr = XFS_AGB_TO_DADDR(mp, cur->bc_ag.pag->pag_agno,
			rec->rc_startblock + rec->rc_blockcount);

	return xfs_fsrefs_helper(cur->bc_tp, info, rec, rec_daddr, 0);
}

/* Execute a getfsrefs query against the regular data device. */
STATIC int
xfs_fsrefs_ddev(
	struct xfs_trans	*tp,
	const struct xfs_fsrefs	*keys,
	struct xfs_fsrefs_info	*info)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_buf		*agf_bp = NULL;
	struct xfs_perag	*pag = NULL;
	xfs_fsblock_t		start_fsb;
	xfs_fsblock_t		end_fsb;
	xfs_agnumber_t		start_ag;
	xfs_agnumber_t		end_ag;
	xfs_agnumber_t		agno;
	uint64_t		eofs;
	int			error = 0;

	eofs = XFS_FSB_TO_BB(mp, mp->m_sb.sb_dblocks);
	if (keys[0].fcr_physical >= eofs)
		return 0;
	start_fsb = XFS_DADDR_TO_FSB(mp, keys[0].fcr_physical);
	end_fsb = XFS_DADDR_TO_FSB(mp, min(eofs - 1, keys[1].fcr_physical));

	info->refc_cur = info->bno_cur = NULL;

	/*
	 * Convert the fsrefs low/high keys to AG based keys.  Initialize
	 * low to the fsrefs low key and max out the high key to the end
	 * of the AG.
	 */
	info->low.rc_startblock = XFS_FSB_TO_AGBNO(mp, start_fsb);
	info->low.rc_blockcount = XFS_BB_TO_FSBT(mp, keys[0].fcr_length);
	info->low.rc_refcount = 0;
	info->low.rc_domain = XFS_REFC_DOMAIN_SHARED;

	/* Adjust the low key if we are continuing from where we left off. */
	if (info->low.rc_blockcount > 0) {
		info->low.rc_startblock += info->low.rc_blockcount;

		start_fsb += info->low.rc_blockcount;
		if (XFS_FSB_TO_DADDR(mp, start_fsb) >= eofs)
			return 0;
	}

	info->high.rc_startblock = -1U;
	info->high.rc_blockcount = 0;
	info->high.rc_refcount = 0;
	info->high.rc_domain = XFS_REFC_DOMAIN_SHARED;

	start_ag = XFS_FSB_TO_AGNO(mp, start_fsb);
	end_ag = XFS_FSB_TO_AGNO(mp, end_fsb);

	/* Query each AG */
	agno = start_ag;
	for_each_perag_range(mp, agno, end_ag, pag) {
		/*
		 * Set the AG high key from the fsrefs high key if this
		 * is the last AG that we're querying.
		 */
		info->pag = pag;
		if (pag->pag_agno == end_ag)
			info->high.rc_startblock = XFS_FSB_TO_AGBNO(mp,
					end_fsb);

		if (info->refc_cur) {
			xfs_btree_del_cursor(info->refc_cur, XFS_BTREE_NOERROR);
			info->refc_cur = NULL;
		}
		if (info->bno_cur) {
			xfs_btree_del_cursor(info->bno_cur, XFS_BTREE_NOERROR);
			info->bno_cur = NULL;
		}
		if (agf_bp) {
			xfs_trans_brelse(tp, agf_bp);
			agf_bp = NULL;
		}

		error = xfs_alloc_read_agf(pag, tp, 0, &agf_bp);
		if (error)
			break;

		trace_xfs_fsrefs_low_key(mp, info->dev, pag->pag_agno,
				&info->low);
		trace_xfs_fsrefs_high_key(mp, info->dev, pag->pag_agno,
				&info->high);

		info->bno_cur = xfs_allocbt_init_cursor(mp, tp, agf_bp, pag,
						XFS_BTNUM_BNO);

		if (xfs_has_reflink(mp)) {
			info->refc_cur = xfs_refcountbt_init_cursor(mp, tp,
							agf_bp, pag);

			/*
			 * Fill the query with refcount records and synthesize
			 * singly-owned block records from free space data.
			 */
			error = xfs_refcount_query_range(info->refc_cur,
					&info->low, &info->high,
					xfs_fsrefs_ddev_refcountbt_helper,
					info);
			if (error && error != -ECANCELED)
				break;
		}

		/*
		 * Synthesize refcount==1 records from the free space data
		 * between the end of the last fsrefs record reported and the
		 * end of the range.  If we don't have refcount support, the
		 * starting point will be the start of the query range.
		 */
		error = xfs_fsrefs_ddev_fill_refcount_gap(tp, info,
				info->high.rc_startblock);
		if (error)
			break;

		/*
		 * Set the AG low key to the start of the AG prior to
		 * moving on to the next AG.
		 */
		if (pag->pag_agno == start_ag)
			memset(&info->low, 0, sizeof(info->low));

		info->pag = NULL;
	}

	if (info->refc_cur) {
		xfs_btree_del_cursor(info->refc_cur, error);
		info->refc_cur = NULL;
	}
	if (info->bno_cur) {
		xfs_btree_del_cursor(info->bno_cur, error);
		info->bno_cur = NULL;
	}
	if (agf_bp)
		xfs_trans_brelse(tp, agf_bp);
	if (info->pag) {
		xfs_perag_rele(info->pag);
		info->pag = NULL;
	} else if (pag) {
		/* loop termination case */
		xfs_perag_rele(pag);
	}

	return error;
}

/* Execute a getfsrefs query against the log device. */
STATIC int
xfs_fsrefs_logdev(
	struct xfs_trans		*tp,
	const struct xfs_fsrefs		*keys,
	struct xfs_fsrefs_info		*info)
{
	struct xfs_mount		*mp = tp->t_mountp;
	struct xfs_refcount_irec	refc;
	xfs_daddr_t			rec_daddr, len_daddr;
	xfs_fsblock_t			start_fsb, end_fsb;
	uint64_t			eofs;

	eofs = XFS_FSB_TO_BB(mp, mp->m_sb.sb_logblocks);
	if (keys[0].fcr_physical >= eofs)
		return 0;
	start_fsb = XFS_BB_TO_FSBT(mp,
				keys[0].fcr_physical + keys[0].fcr_length);
	end_fsb = XFS_BB_TO_FSB(mp, min(eofs - 1, keys[1].fcr_physical));

	/* Adjust the low key if we are continuing from where we left off. */
	if (keys[0].fcr_length > 0)
		info->low_daddr = XFS_FSB_TO_BB(mp, start_fsb);

	trace_xfs_fsrefs_low_key_linear(mp, info->dev, start_fsb);
	trace_xfs_fsrefs_high_key_linear(mp, info->dev, end_fsb);

	if (start_fsb > 0)
		return 0;

	/* Fabricate an refc entry for the external log device. */
	refc.rc_startblock = 0;
	refc.rc_blockcount = mp->m_sb.sb_logblocks;
	refc.rc_refcount = 1;
	refc.rc_domain = XFS_REFC_DOMAIN_SHARED;

	rec_daddr = XFS_FSB_TO_BB(mp, refc.rc_startblock);
	len_daddr = XFS_FSB_TO_BB(mp, refc.rc_blockcount);
	return xfs_fsrefs_helper(tp, info, &refc, rec_daddr, len_daddr);
}

#ifdef CONFIG_XFS_RT
/* Synthesize fsrefs records from rtbitmap records. */
STATIC int
xfs_fsrefs_rtdev_bitmap_helper(
	struct xfs_mount		*mp,
	struct xfs_trans		*tp,
	const struct xfs_rtalloc_rec	*rec,
	void				*priv)
{
	struct xfs_refcount_irec	irec;
	struct xfs_fsrefs_info		*info = priv;
	xfs_rtblock_t			rt_startblock;
	xfs_rtblock_t			rec_rtlen;
	xfs_rtblock_t			next_rtbno;
	xfs_daddr_t			rec_daddr, len_daddr;

	/*
	 * Figure out if there's a gap between the last fsrefs record we
	 * emitted and this free extent.  If there is, report the gap as a
	 * refcount==1 record.
	 */
	next_rtbno = XFS_BB_TO_FSBT(mp, info->next_daddr);
	rec_daddr = XFS_FSB_TO_BB(mp, next_rtbno);
	irec.rc_startblock = next_rtbno;

	rt_startblock = xfs_rtx_to_rtb(mp, rec->ar_startext);
	rec_rtlen = xfs_rtx_to_rtb(mp, rec->ar_extcount);

	ASSERT(rt_startblock >= next_rtbno);

	info->next_daddr = XFS_FSB_TO_BB(mp, rt_startblock + rec_rtlen);

	if (rt_startblock == next_rtbno)
		return 0;

	/* Emit a record for the in-use space */
	irec.rc_blockcount = rt_startblock - next_rtbno;
	len_daddr = XFS_FSB_TO_BB(mp, rt_startblock - next_rtbno);

	irec.rc_refcount = 1;
	irec.rc_domain = XFS_REFC_DOMAIN_SHARED;

	return xfs_fsrefs_helper(tp, info, &irec, rec_daddr, len_daddr);
}

/* Emit records to fill a gap in the refcount btree with singly-owned blocks. */
STATIC int
xfs_fsrefs_rtdev_fill_refcount_gap(
	struct xfs_trans	*tp,
	struct xfs_fsrefs_info	*info,
	xfs_rtblock_t		start_rtb,
	xfs_rtblock_t		end_rtb)
{
	struct xfs_rtalloc_rec	low = { 0 };
	struct xfs_rtalloc_rec	high = { 0 };
	struct xfs_mount	*mp = tp->t_mountp;
	xfs_daddr_t		rec_daddr;
	int			error;

	/*
	 * Set up query parameters to return free extents covering the range we
	 * want.
	 */
	low.ar_startext = xfs_rtb_to_rtx(mp, start_rtb);
	high.ar_startext = xfs_rtb_to_rtxup(mp, end_rtb);
	error = xfs_rtalloc_query_range(mp, tp, &low, &high,
			xfs_fsrefs_rtdev_bitmap_helper, info);
	if (error)
		return error;

	/*
	 * Synthesize records for single-owner extents between the last
	 * fsrefcount record emitted and the end of the query range.
	 */
	high.ar_startext = min(mp->m_sb.sb_rextents, high.ar_startext);
	rec_daddr = XFS_FSB_TO_BB(mp, xfs_rtx_to_rtb(mp, high.ar_startext));
	if (info->next_daddr > rec_daddr)
		return 0;

	info->last = true;
	return xfs_fsrefs_rtdev_bitmap_helper(mp, tp, &high, info);
}

/* Transform a absolute-startblock refcount (rtdev, logdev) into a fsrefs */
STATIC int
xfs_fsrefs_rtdev_refcountbt_helper(
	struct xfs_btree_cur		*cur,
	const struct xfs_refcount_irec	*rec,
	void				*priv)
{
	struct xfs_mount		*mp = cur->bc_mp;
	struct xfs_fsrefs_info		*info = priv;
	xfs_rtblock_t			rec_rtbno, next_rtbno;
	int				error;

	/*
	 * Stop once we get to the CoW staging extents; they're all shoved to
	 * the right side of the btree and were already covered by the rtbitmap
	 * scan.
	 */
	if (rec->rc_domain != XFS_REFC_DOMAIN_SHARED)
		return -ECANCELED;

	/* Report on any gaps first */
	rec_rtbno = xfs_rgbno_to_rtb(mp, cur->bc_ino.rtg->rtg_rgno,
			rec->rc_startblock);
	next_rtbno = XFS_BB_TO_FSBT(mp, info->next_daddr);
	error = xfs_fsrefs_rtdev_fill_refcount_gap(cur->bc_tp, info,
			next_rtbno, rec_rtbno);
	if (error)
		return error;

	/* Report this refcount extent. */
	info->next_daddr = XFS_FSB_TO_BB(mp, rec_rtbno + rec->rc_blockcount);
	return xfs_fsrefs_helper(cur->bc_tp, info, rec,
			XFS_FSB_TO_BB(mp, rec_rtbno), 0);
}

/* Execute a getfsrefs query against the realtime bitmap. */
STATIC int
xfs_fsrefs_rtdev_rtbitmap(
	struct xfs_trans	*tp,
	const struct xfs_fsrefs	*keys,
	struct xfs_fsrefs_info	*info)
{
	struct xfs_mount	*mp = tp->t_mountp;
	xfs_rtblock_t		start_rtb;
	xfs_rtblock_t		end_rtb;
	uint64_t		eofs;
	int			error;

	eofs = XFS_FSB_TO_BB(mp, mp->m_sb.sb_rblocks);
	if (keys[0].fcr_physical >= eofs)
		return 0;
	start_rtb = XFS_BB_TO_FSBT(mp,
				keys[0].fcr_physical + keys[0].fcr_length);
	end_rtb = XFS_BB_TO_FSB(mp, min(eofs - 1, keys[1].fcr_physical));

	info->refc_cur = NULL;

	/* Adjust the low key if we are continuing from where we left off. */
	if (keys[0].fcr_length > 0) {
		info->low_daddr = XFS_FSB_TO_BB(mp, start_rtb);
		if (info->low_daddr >= eofs)
			return 0;
	}

	trace_xfs_fsrefs_low_key_linear(mp, info->dev, start_rtb);
	trace_xfs_fsrefs_high_key_linear(mp, info->dev, end_rtb);

	/* Synthesize refcount==1 records from the free space data. */
	xfs_rtbitmap_lock_shared(mp, XFS_RBMLOCK_BITMAP);
	error = xfs_fsrefs_rtdev_fill_refcount_gap(tp, info, start_rtb,
			end_rtb);
	xfs_rtbitmap_unlock_shared(mp, XFS_RBMLOCK_BITMAP);
	return error;
}

#define XFS_RTGLOCK_FSREFS	(XFS_RTGLOCK_BITMAP_SHARED | \
				 XFS_RTGLOCK_REFCOUNT)

/* Execute a getfsrefs query against the realtime device. */
STATIC int
xfs_fsrefs_rtdev(
	struct xfs_trans	*tp,
	const struct xfs_fsrefs	*keys,
	struct xfs_fsrefs_info	*info)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_rtgroup	*rtg;
	xfs_rtblock_t		start_rtb;
	xfs_rtblock_t		end_rtb;
	uint64_t		eofs;
	xfs_rgnumber_t		start_rg, end_rg;
	int			error = 0;

	eofs = XFS_FSB_TO_BB(mp, xfs_rtx_to_rtb(mp, mp->m_sb.sb_rextents));
	if (keys[0].fcr_physical >= eofs)
		return 0;
	start_rtb = XFS_BB_TO_FSBT(mp, keys[0].fcr_physical);
	end_rtb = XFS_BB_TO_FSB(mp, min(eofs - 1, keys[1].fcr_physical));

	info->refc_cur = NULL;

	/*
	 * Convert the fsrefs low/high keys to rtgroup based keys.  Initialize
	 * low to the fsrefs low key and max out the high key to the end of the
	 * rtgroup.
	 */
	info->low.rc_startblock = xfs_rtb_to_rgbno(mp, start_rtb, &start_rg);
	info->low.rc_blockcount = XFS_BB_TO_FSBT(mp, keys[0].fcr_length);
	info->low.rc_refcount = 0;
	info->low.rc_domain = XFS_REFC_DOMAIN_SHARED;

	/* Adjust the low key if we are continuing from where we left off. */
	if (info->low.rc_blockcount > 0) {
		info->low.rc_startblock += info->low.rc_blockcount;

		start_rtb += info->low.rc_blockcount;
		if (xfs_rtb_to_daddr(mp, start_rtb) >= eofs)
			return 0;
	}

	info->high.rc_startblock = -1U;
	info->high.rc_blockcount = 0;
	info->high.rc_refcount = 0;
	info->high.rc_domain = XFS_REFC_DOMAIN_SHARED;

	end_rg = xfs_rtb_to_rgno(mp, end_rtb);

	for_each_rtgroup_range(mp, start_rg, end_rg, rtg) {
		/*
		 * Set the rtgroup high key from the fsrefs high key if this
		 * is the last rtgroup that we're querying.
		 */
		info->rtg = rtg;
		if (rtg->rtg_rgno == end_rg) {
			xfs_rgnumber_t	junk;

			info->high.rc_startblock = xfs_rtb_to_rgbno(mp,
					end_rtb, &junk);
		}

		if (info->refc_cur) {
			xfs_rtgroup_unlock(info->refc_cur->bc_ino.rtg,
					XFS_RTGLOCK_FSREFS);
			xfs_btree_del_cursor(info->refc_cur, XFS_BTREE_NOERROR);
			info->refc_cur = NULL;
		}

		trace_xfs_fsrefs_low_key(mp, info->dev, rtg->rtg_rgno,
				&info->low);
		trace_xfs_fsrefs_high_key(mp, info->dev, rtg->rtg_rgno,
				&info->high);

		xfs_rtgroup_lock(NULL, rtg, XFS_RTGLOCK_FSREFS);
		info->refc_cur = xfs_rtrefcountbt_init_cursor(mp, tp, rtg,
						rtg->rtg_refcountip);

		/*
		 * Fill the query with refcount records and synthesize
		 * singly-owned block records from free space data.
		 */
		error = xfs_refcount_query_range(info->refc_cur,
				&info->low, &info->high,
				xfs_fsrefs_rtdev_refcountbt_helper, info);
		if (error && error != -ECANCELED)
			break;

		/*
		 * Set the rtgroup low key to the start of the rtgroup prior to
		 * moving on to the next rtgroup.
		 */
		if (rtg->rtg_rgno == start_rg)
			memset(&info->low, 0, sizeof(info->low));

		/*
		 * If this is the last rtgroup, report any gap at the end of it
		 * before we drop the reference to the perag when the loop
		 * terminates.
		 */
		if (rtg->rtg_rgno == end_rg) {
			xfs_rtblock_t	next_rtbno;

			info->last = true;
			next_rtbno = XFS_BB_TO_FSBT(mp, info->next_daddr);
			error = xfs_fsrefs_rtdev_fill_refcount_gap(tp, info,
					next_rtbno, end_rtb);
			if (error)
				break;
		}
		info->rtg = NULL;
	}

	if (info->refc_cur) {
		xfs_rtgroup_unlock(info->refc_cur->bc_ino.rtg,
				XFS_RTGLOCK_FSREFS);
		xfs_btree_del_cursor(info->refc_cur, error);
		info->refc_cur = NULL;
	}
	if (info->rtg) {
		xfs_rtgroup_rele(info->rtg);
		info->rtg = NULL;
	} else if (rtg) {
		/* loop termination case */
		xfs_rtgroup_rele(rtg);
	}

	return error;
}
#endif

/* Do we recognize the device? */
STATIC bool
xfs_fsrefs_is_valid_device(
	struct xfs_mount	*mp,
	struct xfs_fsrefs	*fcr)
{
	if (fcr->fcr_device == 0 || fcr->fcr_device == UINT_MAX ||
	    fcr->fcr_device == new_encode_dev(mp->m_ddev_targp->bt_dev))
		return true;
	if (mp->m_logdev_targp &&
	    fcr->fcr_device == new_encode_dev(mp->m_logdev_targp->bt_dev))
		return true;
	if (mp->m_rtdev_targp &&
	    fcr->fcr_device == new_encode_dev(mp->m_rtdev_targp->bt_dev))
		return true;
	return false;
}

/* Ensure that the low key is less than the high key. */
STATIC bool
xfs_fsrefs_check_keys(
	struct xfs_fsrefs	*low_key,
	struct xfs_fsrefs	*high_key)
{
	if (low_key->fcr_device > high_key->fcr_device)
		return false;
	if (low_key->fcr_device < high_key->fcr_device)
		return true;

	if (low_key->fcr_physical > high_key->fcr_physical)
		return false;
	if (low_key->fcr_physical < high_key->fcr_physical)
		return true;

	return false;
}

/*
 * There are only two devices if we didn't configure RT devices at build time.
 */
#ifdef CONFIG_XFS_RT
#define XFS_GETFSREFS_DEVS	3
#else
#define XFS_GETFSREFS_DEVS	2
#endif /* CONFIG_XFS_RT */

/*
 * Get filesystem's extent refcounts as described in head, and format for
 * output. Fills in the supplied records array until there are no more reverse
 * mappings to return or head.fch_entries == head.fch_count.  In the second
 * case, this function returns -ECANCELED to indicate that more records would
 * have been returned.
 *
 * Key to Confusion
 * ----------------
 * There are multiple levels of keys and counters at work here:
 * xfs_fsrefs_head.fch_keys	-- low and high fsrefs keys passed in;
 *				   these reflect fs-wide sector addrs.
 * dkeys			-- fch_keys used to query each device;
 *				   these are fch_keys but w/ the low key
 *				   bumped up by fcr_length.
 * xfs_fsrefs_info.next_daddr-- next disk addr we expect to see; this
 *				   is how we detect gaps in the fsrefs
 *				   records and report them.
 * xfs_fsrefs_info.low/high	-- per-AG low/high keys computed from
 *				   dkeys; used to query the metadata.
 */
int
xfs_getfsrefs(
	struct xfs_mount	*mp,
	struct xfs_fsrefs_head	*head,
	struct xfs_getfsrefs	*fsrefs_recs)
{
	struct xfs_trans	*tp = NULL;
	struct xfs_fsrefs	dkeys[2];	/* per-dev keys */
	struct xfs_fsrefs_dev	handlers[XFS_GETFSREFS_DEVS];
	struct xfs_fsrefs_info	info = { NULL };
	int			i;
	int			error = 0;

	if (head->fch_iflags & ~FCH_IF_VALID)
		return -EINVAL;
	if (!xfs_fsrefs_is_valid_device(mp, &head->fch_keys[0]) ||
	    !xfs_fsrefs_is_valid_device(mp, &head->fch_keys[1]))
		return -EINVAL;
	if (!xfs_fsrefs_check_keys(&head->fch_keys[0], &head->fch_keys[1]))
		return -EINVAL;

	head->fch_entries = 0;

	/* Set up our device handlers. */
	memset(handlers, 0, sizeof(handlers));
	handlers[0].dev = new_encode_dev(mp->m_ddev_targp->bt_dev);
	handlers[0].fn = xfs_fsrefs_ddev;
	if (mp->m_logdev_targp != mp->m_ddev_targp) {
		handlers[1].dev = new_encode_dev(mp->m_logdev_targp->bt_dev);
		handlers[1].fn = xfs_fsrefs_logdev;
	}
#ifdef CONFIG_XFS_RT
	if (mp->m_rtdev_targp) {
		handlers[2].dev = new_encode_dev(mp->m_rtdev_targp->bt_dev);
		if (xfs_has_rtreflink(mp))
			handlers[2].fn = xfs_fsrefs_rtdev;
		else
			handlers[2].fn = xfs_fsrefs_rtdev_rtbitmap;
	}
#endif /* CONFIG_XFS_RT */

	xfs_sort(handlers, XFS_GETFSREFS_DEVS, sizeof(struct xfs_fsrefs_dev),
			xfs_fsrefs_dev_compare);

	/*
	 * To continue where we left off, we allow userspace to use the last
	 * mapping from a previous call as the low key of the next.  This is
	 * identified by a non-zero length in the low key. We have to increment
	 * the low key in this scenario to ensure we don't return the same
	 * mapping again, and instead return the very next mapping.  Bump the
	 * physical offset as there can be no other mapping for the same
	 * physical block range.
	 *
	 * Each fsrefs backend is responsible for making this adjustment as
	 * appropriate for the backend.
	 */
	dkeys[0] = head->fch_keys[0];
	memset(&dkeys[1], 0xFF, sizeof(struct xfs_fsrefs));

	info.next_daddr = head->fch_keys[0].fcr_physical +
			  head->fch_keys[0].fcr_length;
	info.fsrefs_recs = fsrefs_recs;
	info.head = head;

	/* For each device we support... */
	for (i = 0; i < XFS_GETFSREFS_DEVS; i++) {
		/* Is this device within the range the user asked for? */
		if (!handlers[i].fn)
			continue;
		if (head->fch_keys[0].fcr_device > handlers[i].dev)
			continue;
		if (head->fch_keys[1].fcr_device < handlers[i].dev)
			break;

		/*
		 * If this device number matches the high key, we have to pass
		 * the high key to the handler to limit the query results.  If
		 * the device number exceeds the low key, zero out the low key
		 * so that we get everything from the beginning.
		 */
		if (handlers[i].dev == head->fch_keys[1].fcr_device)
			dkeys[1] = head->fch_keys[1];
		if (handlers[i].dev > head->fch_keys[0].fcr_device)
			memset(&dkeys[0], 0, sizeof(struct xfs_fsrefs));

		/*
		 * Grab an empty transaction so that we can use its recursive
		 * buffer locking abilities to detect cycles in the refcountbt
		 * without deadlocking.
		 */
		error = xfs_trans_alloc_empty(mp, &tp);
		if (error)
			break;

		info.dev = handlers[i].dev;
		info.last = false;
		info.pag = NULL;
		info.rtg = NULL;
		info.low_daddr = -1ULL;
		info.low.rc_blockcount = 0;
		error = handlers[i].fn(tp, dkeys, &info);
		if (error)
			break;
		xfs_trans_cancel(tp);
		tp = NULL;
		info.next_daddr = 0;
	}

	if (tp)
		xfs_trans_cancel(tp);
	head->fch_oflags = FCH_OF_DEV_T;
	return error;
}
