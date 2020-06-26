// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * Copyright (c) 2012 Red Hat, Inc.
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
#include "xfs_defer.h"
#include "xfs_inode.h"
#include "xfs_btree.h"
#include "xfs_trans.h"
#include "xfs_alloc.h"
#include "xfs_bmap.h"
#include "xfs_bmap_util.h"
#include "xfs_bmap_btree.h"
#include "xfs_rtalloc.h"
#include "xfs_error.h"
#include "xfs_quota.h"
#include "xfs_trans_space.h"
#include "xfs_trace.h"
#include "xfs_icache.h"
#include "xfs_iomap.h"
#include "xfs_reflink.h"
#include "xfs_sb.h"
#include "xfs_swapext.h"

/* Kernel only BMAP related definitions and functions */

/*
 * Convert the given file system block to a disk block.  We have to treat it
 * differently based on whether the file is a real time file or not, because the
 * bmap code does.
 */
xfs_daddr_t
xfs_fsb_to_db(struct xfs_inode *ip, xfs_fsblock_t fsb)
{
	if (XFS_IS_REALTIME_INODE(ip))
		return XFS_FSB_TO_BB(ip->i_mount, fsb);
	return XFS_FSB_TO_DADDR(ip->i_mount, fsb);
}

/*
 * Routine to zero an extent on disk allocated to the specific inode.
 *
 * The VFS functions take a linearised filesystem block offset, so we have to
 * convert the sparse xfs fsb to the right format first.
 * VFS types are real funky, too.
 */
int
xfs_zero_extent(
	struct xfs_inode	*ip,
	xfs_fsblock_t		start_fsb,
	xfs_off_t		count_fsb)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_buftarg	*target = xfs_inode_buftarg(ip);
	xfs_daddr_t		sector = xfs_fsb_to_db(ip, start_fsb);
	sector_t		block = XFS_BB_TO_FSBT(mp, sector);

	return blkdev_issue_zeroout(target->bt_bdev,
		block << (mp->m_super->s_blocksize_bits - 9),
		count_fsb << (mp->m_super->s_blocksize_bits - 9),
		GFP_NOFS, 0);
}

#ifdef CONFIG_XFS_RT
int
xfs_bmap_rtalloc(
	struct xfs_bmalloca	*ap)	/* bmap alloc argument struct */
{
	int		error;		/* error return value */
	xfs_mount_t	*mp;		/* mount point structure */
	xfs_extlen_t	prod = 0;	/* product factor for allocators */
	xfs_extlen_t	mod = 0;	/* product factor for allocators */
	xfs_extlen_t	ralen = 0;	/* realtime allocation length */
	xfs_extlen_t	align;		/* minimum allocation alignment */
	xfs_rtblock_t	rtb;

	mp = ap->ip->i_mount;
	align = xfs_get_extsz_hint(ap->ip);
	prod = align / mp->m_sb.sb_rextsize;
	error = xfs_bmap_extsize_align(mp, &ap->got, &ap->prev,
					align, 1, ap->eof, 0,
					ap->conv, &ap->offset, &ap->length);
	if (error)
		return error;
	ASSERT(ap->length);
	ASSERT(ap->length % mp->m_sb.sb_rextsize == 0);

	/*
	 * If the offset & length are not perfectly aligned
	 * then kill prod, it will just get us in trouble.
	 */
	div_u64_rem(ap->offset, align, &mod);
	if (mod || ap->length % align)
		prod = 1;
	/*
	 * Set ralen to be the actual requested length in rtextents.
	 */
	ralen = ap->length / mp->m_sb.sb_rextsize;
	/*
	 * If the old value was close enough to MAXEXTLEN that
	 * we rounded up to it, cut it back so it's valid again.
	 * Note that if it's a really large request (bigger than
	 * MAXEXTLEN), we don't hear about that number, and can't
	 * adjust the starting point to match it.
	 */
	if (ralen * mp->m_sb.sb_rextsize >= MAXEXTLEN)
		ralen = MAXEXTLEN / mp->m_sb.sb_rextsize;

	/*
	 * Lock out modifications to both the RT bitmap and summary inodes
	 */
	xfs_ilock(mp->m_rbmip, XFS_ILOCK_EXCL|XFS_ILOCK_RTBITMAP);
	xfs_trans_ijoin(ap->tp, mp->m_rbmip, XFS_ILOCK_EXCL);
	xfs_ilock(mp->m_rsumip, XFS_ILOCK_EXCL|XFS_ILOCK_RTSUM);
	xfs_trans_ijoin(ap->tp, mp->m_rsumip, XFS_ILOCK_EXCL);

	/*
	 * If it's an allocation to an empty file at offset 0,
	 * pick an extent that will space things out in the rt area.
	 */
	if (ap->eof && ap->offset == 0) {
		xfs_rtblock_t uninitialized_var(rtx); /* realtime extent no */

		error = xfs_rtpick_extent(mp, ap->tp, ralen, &rtx);
		if (error)
			return error;
		ap->blkno = rtx * mp->m_sb.sb_rextsize;
	} else {
		ap->blkno = 0;
	}

	xfs_bmap_adjacent(ap);

	/*
	 * Realtime allocation, done through xfs_rtallocate_extent.
	 */
	do_div(ap->blkno, mp->m_sb.sb_rextsize);
	rtb = ap->blkno;
	ap->length = ralen;
	error = xfs_rtallocate_extent(ap->tp, ap->blkno, 1, ap->length,
				&ralen, ap->wasdel, prod, &rtb);
	if (error)
		return error;

	ap->blkno = rtb;
	if (ap->blkno != NULLFSBLOCK) {
		ap->blkno *= mp->m_sb.sb_rextsize;
		ralen *= mp->m_sb.sb_rextsize;
		ap->length = ralen;
		ap->ip->i_d.di_nblocks += ralen;
		xfs_trans_log_inode(ap->tp, ap->ip, XFS_ILOG_CORE);
		if (ap->wasdel)
			ap->ip->i_delayed_blks -= ralen;
		/*
		 * Adjust the disk quota also. This was reserved
		 * earlier.
		 */
		xfs_trans_mod_dquot_byino(ap->tp, ap->ip,
			ap->wasdel ? XFS_TRANS_DQ_DELRTBCOUNT :
					XFS_TRANS_DQ_RTBCOUNT, (long) ralen);
	} else {
		ap->length = 0;
	}
	return 0;
}
#endif /* CONFIG_XFS_RT */

/*
 * Extent tree block counting routines.
 */

/*
 * Count leaf blocks given a range of extent records.  Delayed allocation
 * extents are not counted towards the totals.
 */
xfs_extnum_t
xfs_bmap_count_leaves(
	struct xfs_ifork	*ifp,
	xfs_filblks_t		*count)
{
	struct xfs_iext_cursor	icur;
	struct xfs_bmbt_irec	got;
	xfs_extnum_t		numrecs = 0;

	for_each_xfs_iext(ifp, &icur, &got) {
		if (!isnullstartblock(got.br_startblock)) {
			*count += got.br_blockcount;
			numrecs++;
		}
	}

	return numrecs;
}

/*
 * Count fsblocks of the given fork.  Delayed allocation extents are
 * not counted towards the totals.
 */
int
xfs_bmap_count_blocks(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			whichfork,
	xfs_extnum_t		*nextents,
	xfs_filblks_t		*count)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_ifork	*ifp = XFS_IFORK_PTR(ip, whichfork);
	struct xfs_btree_cur	*cur;
	xfs_extlen_t		btblocks = 0;
	int			error;

	*nextents = 0;
	*count = 0;

	if (!ifp)
		return 0;

	switch (ifp->if_format) {
	case XFS_DINODE_FMT_BTREE:
		if (!(ifp->if_flags & XFS_IFEXTENTS)) {
			error = xfs_iread_extents(tp, ip, whichfork);
			if (error)
				return error;
		}

		cur = xfs_bmbt_init_cursor(mp, tp, ip, whichfork);
		error = xfs_btree_count_blocks(cur, &btblocks);
		xfs_btree_del_cursor(cur, error);
		if (error)
			return error;

		/*
		 * xfs_btree_count_blocks includes the root block contained in
		 * the inode fork in @btblocks, so subtract one because we're
		 * only interested in allocated disk blocks.
		 */
		*count += btblocks - 1;

		/* fall through */
	case XFS_DINODE_FMT_EXTENTS:
		*nextents = xfs_bmap_count_leaves(ifp, count);
		break;
	}

	return 0;
}

static int
xfs_getbmap_report_one(
	struct xfs_inode	*ip,
	struct getbmapx		*bmv,
	struct kgetbmap		*out,
	int64_t			bmv_end,
	struct xfs_bmbt_irec	*got)
{
	struct kgetbmap		*p = out + bmv->bmv_entries;
	bool			shared = false;
	int			error;

	error = xfs_reflink_trim_around_shared(ip, got, &shared);
	if (error)
		return error;

	if (isnullstartblock(got->br_startblock) ||
	    got->br_startblock == DELAYSTARTBLOCK) {
		/*
		 * Delalloc extents that start beyond EOF can occur due to
		 * speculative EOF allocation when the delalloc extent is larger
		 * than the largest freespace extent at conversion time.  These
		 * extents cannot be converted by data writeback, so can exist
		 * here even if we are not supposed to be finding delalloc
		 * extents.
		 */
		if (got->br_startoff < XFS_B_TO_FSB(ip->i_mount, XFS_ISIZE(ip)))
			ASSERT((bmv->bmv_iflags & BMV_IF_DELALLOC) != 0);

		p->bmv_oflags |= BMV_OF_DELALLOC;
		p->bmv_block = -2;
	} else {
		p->bmv_block = xfs_fsb_to_db(ip, got->br_startblock);
	}

	if (got->br_state == XFS_EXT_UNWRITTEN &&
	    (bmv->bmv_iflags & BMV_IF_PREALLOC))
		p->bmv_oflags |= BMV_OF_PREALLOC;

	if (shared)
		p->bmv_oflags |= BMV_OF_SHARED;

	p->bmv_offset = XFS_FSB_TO_BB(ip->i_mount, got->br_startoff);
	p->bmv_length = XFS_FSB_TO_BB(ip->i_mount, got->br_blockcount);

	bmv->bmv_offset = p->bmv_offset + p->bmv_length;
	bmv->bmv_length = max(0LL, bmv_end - bmv->bmv_offset);
	bmv->bmv_entries++;
	return 0;
}

static void
xfs_getbmap_report_hole(
	struct xfs_inode	*ip,
	struct getbmapx		*bmv,
	struct kgetbmap		*out,
	int64_t			bmv_end,
	xfs_fileoff_t		bno,
	xfs_fileoff_t		end)
{
	struct kgetbmap		*p = out + bmv->bmv_entries;

	if (bmv->bmv_iflags & BMV_IF_NO_HOLES)
		return;

	p->bmv_block = -1;
	p->bmv_offset = XFS_FSB_TO_BB(ip->i_mount, bno);
	p->bmv_length = XFS_FSB_TO_BB(ip->i_mount, end - bno);

	bmv->bmv_offset = p->bmv_offset + p->bmv_length;
	bmv->bmv_length = max(0LL, bmv_end - bmv->bmv_offset);
	bmv->bmv_entries++;
}

static inline bool
xfs_getbmap_full(
	struct getbmapx		*bmv)
{
	return bmv->bmv_length == 0 || bmv->bmv_entries >= bmv->bmv_count - 1;
}

static bool
xfs_getbmap_next_rec(
	struct xfs_bmbt_irec	*rec,
	xfs_fileoff_t		total_end)
{
	xfs_fileoff_t		end = rec->br_startoff + rec->br_blockcount;

	if (end == total_end)
		return false;

	rec->br_startoff += rec->br_blockcount;
	if (!isnullstartblock(rec->br_startblock) &&
	    rec->br_startblock != DELAYSTARTBLOCK)
		rec->br_startblock += rec->br_blockcount;
	rec->br_blockcount = total_end - end;
	return true;
}

/*
 * Get inode's extents as described in bmv, and format for output.
 * Calls formatter to fill the user's buffer until all extents
 * are mapped, until the passed-in bmv->bmv_count slots have
 * been filled, or until the formatter short-circuits the loop,
 * if it is tracking filled-in extents on its own.
 */
int						/* error code */
xfs_getbmap(
	struct xfs_inode	*ip,
	struct getbmapx		*bmv,		/* user bmap structure */
	struct kgetbmap		*out)
{
	struct xfs_mount	*mp = ip->i_mount;
	int			iflags = bmv->bmv_iflags;
	int			whichfork, lock, error = 0;
	int64_t			bmv_end, max_len;
	xfs_fileoff_t		bno, first_bno;
	struct xfs_ifork	*ifp;
	struct xfs_bmbt_irec	got, rec;
	xfs_filblks_t		len;
	struct xfs_iext_cursor	icur;

	if (bmv->bmv_iflags & ~BMV_IF_VALID)
		return -EINVAL;
#ifndef DEBUG
	/* Only allow CoW fork queries if we're debugging. */
	if (iflags & BMV_IF_COWFORK)
		return -EINVAL;
#endif
	if ((iflags & BMV_IF_ATTRFORK) && (iflags & BMV_IF_COWFORK))
		return -EINVAL;

	if (bmv->bmv_length < -1)
		return -EINVAL;
	bmv->bmv_entries = 0;
	if (bmv->bmv_length == 0)
		return 0;

	if (iflags & BMV_IF_ATTRFORK)
		whichfork = XFS_ATTR_FORK;
	else if (iflags & BMV_IF_COWFORK)
		whichfork = XFS_COW_FORK;
	else
		whichfork = XFS_DATA_FORK;
	ifp = XFS_IFORK_PTR(ip, whichfork);

	xfs_ilock(ip, XFS_IOLOCK_SHARED);
	switch (whichfork) {
	case XFS_ATTR_FORK:
		if (!XFS_IFORK_Q(ip))
			goto out_unlock_iolock;

		max_len = 1LL << 32;
		lock = xfs_ilock_attr_map_shared(ip);
		break;
	case XFS_COW_FORK:
		/* No CoW fork? Just return */
		if (!ifp)
			goto out_unlock_iolock;

		if (xfs_get_cowextsz_hint(ip))
			max_len = mp->m_super->s_maxbytes;
		else
			max_len = XFS_ISIZE(ip);

		lock = XFS_ILOCK_SHARED;
		xfs_ilock(ip, lock);
		break;
	case XFS_DATA_FORK:
		if (!(iflags & BMV_IF_DELALLOC) &&
		    (ip->i_delayed_blks || XFS_ISIZE(ip) > ip->i_d.di_size)) {
			error = filemap_write_and_wait(VFS_I(ip)->i_mapping);
			if (error)
				goto out_unlock_iolock;

			/*
			 * Even after flushing the inode, there can still be
			 * delalloc blocks on the inode beyond EOF due to
			 * speculative preallocation.  These are not removed
			 * until the release function is called or the inode
			 * is inactivated.  Hence we cannot assert here that
			 * ip->i_delayed_blks == 0.
			 */
		}

		if (xfs_get_extsz_hint(ip) ||
		    (ip->i_d.di_flags &
		     (XFS_DIFLAG_PREALLOC | XFS_DIFLAG_APPEND)))
			max_len = mp->m_super->s_maxbytes;
		else
			max_len = XFS_ISIZE(ip);

		lock = xfs_ilock_data_map_shared(ip);
		break;
	}

	switch (ifp->if_format) {
	case XFS_DINODE_FMT_EXTENTS:
	case XFS_DINODE_FMT_BTREE:
		break;
	case XFS_DINODE_FMT_LOCAL:
		/* Local format inode forks report no extents. */
		goto out_unlock_ilock;
	default:
		error = -EINVAL;
		goto out_unlock_ilock;
	}

	if (bmv->bmv_length == -1) {
		max_len = XFS_FSB_TO_BB(mp, XFS_B_TO_FSB(mp, max_len));
		bmv->bmv_length = max(0LL, max_len - bmv->bmv_offset);
	}

	bmv_end = bmv->bmv_offset + bmv->bmv_length;

	first_bno = bno = XFS_BB_TO_FSBT(mp, bmv->bmv_offset);
	len = XFS_BB_TO_FSB(mp, bmv->bmv_length);

	if (!(ifp->if_flags & XFS_IFEXTENTS)) {
		error = xfs_iread_extents(NULL, ip, whichfork);
		if (error)
			goto out_unlock_ilock;
	}

	if (!xfs_iext_lookup_extent(ip, ifp, bno, &icur, &got)) {
		/*
		 * Report a whole-file hole if the delalloc flag is set to
		 * stay compatible with the old implementation.
		 */
		if (iflags & BMV_IF_DELALLOC)
			xfs_getbmap_report_hole(ip, bmv, out, bmv_end, bno,
					XFS_B_TO_FSB(mp, XFS_ISIZE(ip)));
		goto out_unlock_ilock;
	}

	while (!xfs_getbmap_full(bmv)) {
		xfs_trim_extent(&got, first_bno, len);

		/*
		 * Report an entry for a hole if this extent doesn't directly
		 * follow the previous one.
		 */
		if (got.br_startoff > bno) {
			xfs_getbmap_report_hole(ip, bmv, out, bmv_end, bno,
					got.br_startoff);
			if (xfs_getbmap_full(bmv))
				break;
		}

		/*
		 * In order to report shared extents accurately, we report each
		 * distinct shared / unshared part of a single bmbt record with
		 * an individual getbmapx record.
		 */
		bno = got.br_startoff + got.br_blockcount;
		rec = got;
		do {
			error = xfs_getbmap_report_one(ip, bmv, out, bmv_end,
					&rec);
			if (error || xfs_getbmap_full(bmv))
				goto out_unlock_ilock;
		} while (xfs_getbmap_next_rec(&rec, bno));

		if (!xfs_iext_next_extent(ifp, &icur, &got)) {
			xfs_fileoff_t	end = XFS_B_TO_FSB(mp, XFS_ISIZE(ip));

			out[bmv->bmv_entries - 1].bmv_oflags |= BMV_OF_LAST;

			if (whichfork != XFS_ATTR_FORK && bno < end &&
			    !xfs_getbmap_full(bmv)) {
				xfs_getbmap_report_hole(ip, bmv, out, bmv_end,
						bno, end);
			}
			break;
		}

		if (bno >= first_bno + len)
			break;
	}

out_unlock_ilock:
	xfs_iunlock(ip, lock);
out_unlock_iolock:
	xfs_iunlock(ip, XFS_IOLOCK_SHARED);
	return error;
}

/*
 * Dead simple method of punching delalyed allocation blocks from a range in
 * the inode.  This will always punch out both the start and end blocks, even
 * if the ranges only partially overlap them, so it is up to the caller to
 * ensure that partial blocks are not passed in.
 */
int
xfs_bmap_punch_delalloc_range(
	struct xfs_inode	*ip,
	xfs_fileoff_t		start_fsb,
	xfs_fileoff_t		length)
{
	struct xfs_ifork	*ifp = &ip->i_df;
	xfs_fileoff_t		end_fsb = start_fsb + length;
	struct xfs_bmbt_irec	got, del;
	struct xfs_iext_cursor	icur;
	int			error = 0;

	ASSERT(ifp->if_flags & XFS_IFEXTENTS);

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	if (!xfs_iext_lookup_extent_before(ip, ifp, &end_fsb, &icur, &got))
		goto out_unlock;

	while (got.br_startoff + got.br_blockcount > start_fsb) {
		del = got;
		xfs_trim_extent(&del, start_fsb, length);

		/*
		 * A delete can push the cursor forward. Step back to the
		 * previous extent on non-delalloc or extents outside the
		 * target range.
		 */
		if (!del.br_blockcount ||
		    !isnullstartblock(del.br_startblock)) {
			if (!xfs_iext_prev_extent(ifp, &icur, &got))
				break;
			continue;
		}

		error = xfs_bmap_del_extent_delay(ip, XFS_DATA_FORK, &icur,
						  &got, &del);
		if (error || !xfs_iext_get_extent(ifp, &icur, &got))
			break;
	}

out_unlock:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}

/*
 * Test whether it is appropriate to check an inode for and free post EOF
 * blocks. The 'force' parameter determines whether we should also consider
 * regular files that are marked preallocated or append-only.
 */
bool
xfs_can_free_eofblocks(struct xfs_inode *ip, bool force)
{
	/* prealloc/delalloc exists only on regular files */
	if (!S_ISREG(VFS_I(ip)->i_mode))
		return false;

	/*
	 * Zero sized files with no cached pages and delalloc blocks will not
	 * have speculative prealloc/delalloc blocks to remove.
	 */
	if (VFS_I(ip)->i_size == 0 &&
	    VFS_I(ip)->i_mapping->nrpages == 0 &&
	    ip->i_delayed_blks == 0)
		return false;

	/* If we haven't read in the extent list, then don't do it now. */
	if (!(ip->i_df.if_flags & XFS_IFEXTENTS))
		return false;

	/*
	 * Do not free real preallocated or append-only files unless the file
	 * has delalloc blocks and we are forced to remove them.
	 */
	if (ip->i_d.di_flags & (XFS_DIFLAG_PREALLOC | XFS_DIFLAG_APPEND))
		if (!force || ip->i_delayed_blks == 0)
			return false;

	return true;
}

/*
 * This is called to free any blocks beyond eof. The caller must hold
 * IOLOCK_EXCL unless we are in the inode reclaim path and have the only
 * reference to the inode.
 */
int
xfs_free_eofblocks(
	struct xfs_inode	*ip)
{
	struct xfs_trans	*tp;
	struct xfs_mount	*mp = ip->i_mount;
	bool			has;
	int			error;

	/*
	 * If there are blocks after the end of file, truncate the file to its
	 * current size to free them up.
	 */
	error = xfs_has_eofblocks(ip, &has);
	if (error || !has)
		return error;

	/*
	 * Attach the dquots to the inode up front.
	 */
	error = xfs_qm_dqattach(ip);
	if (error)
		return error;

	/* wait on dio to ensure i_size has settled */
	inode_dio_wait(VFS_I(ip));

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0, &tp);
	if (error) {
		ASSERT(XFS_FORCED_SHUTDOWN(mp));
		return error;
	}

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, ip, 0);

	/*
	 * Do not update the on-disk file size.  If we update the
	 * on-disk file size and then the system crashes before the
	 * contents of the file are flushed to disk then the files
	 * may be full of holes (ie NULL files bug).
	 */
	error = xfs_itruncate_extents_flags(&tp, ip, XFS_DATA_FORK,
				XFS_ISIZE(ip), XFS_BMAPI_NODISCARD);
	if (error)
		goto err_cancel;

	error = xfs_trans_commit(tp);
	if (error)
		goto out_unlock;

	xfs_inode_clear_eofblocks_tag(ip);
	goto out_unlock;

err_cancel:
	/*
	 * If we get an error at this point we simply don't
	 * bother truncating the file.
	 */
	xfs_trans_cancel(tp);
out_unlock:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}

/*
 * If we suspect that the target device isn't going to be able to satisfy the
 * entire request, try forcing inode inactivation to free up space.  While it's
 * perfectly fine to fill a preallocation request with a bunch of short
 * extents, we'd prefer to do the inactivation work now to combat long term
 * fragmentation in new file data.
 */
static void
xfs_alloc_reclaim_inactive_space(
	struct xfs_mount	*mp,
	bool			is_rt,
	xfs_filblks_t		allocatesize_fsb)
{
	struct xfs_perag	*pag;
	struct xfs_sb		*sbp = &mp->m_sb;
	xfs_extlen_t		free;
	xfs_agnumber_t		agno;

	if (is_rt) {
		if (sbp->sb_frextents * sbp->sb_rextsize >= allocatesize_fsb)
			return;
	} else {
		for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
			pag = xfs_perag_get(mp, agno);
			free = pag->pagf_freeblks;
			xfs_perag_put(pag);

			if (free >= allocatesize_fsb)
				return;
		}
	}

	xfs_inactive_force(mp);
}

int
xfs_alloc_file_space(
	struct xfs_inode	*ip,
	xfs_off_t		offset,
	xfs_off_t		len,
	int			alloc_type)
{
	xfs_mount_t		*mp = ip->i_mount;
	xfs_off_t		count;
	xfs_filblks_t		allocated_fsb;
	xfs_filblks_t		allocatesize_fsb;
	xfs_extlen_t		extsz, temp;
	xfs_fileoff_t		startoffset_fsb;
	xfs_fileoff_t		endoffset_fsb;
	int			nimaps;
	int			quota_flag;
	int			rt;
	xfs_trans_t		*tp;
	xfs_bmbt_irec_t		imaps[1], *imapp;
	uint			qblocks, resblks, resrtextents;
	int			error;

	trace_xfs_alloc_file_space(ip);

	if (XFS_FORCED_SHUTDOWN(mp))
		return -EIO;

	error = xfs_qm_dqattach(ip);
	if (error)
		return error;

	if (len <= 0)
		return -EINVAL;

	rt = XFS_IS_REALTIME_INODE(ip);
	extsz = xfs_get_extsz_hint(ip);

	count = len;
	imapp = &imaps[0];
	nimaps = 1;
	startoffset_fsb	= XFS_B_TO_FSBT(mp, offset);
	endoffset_fsb = XFS_B_TO_FSB(mp, offset + count);
	allocatesize_fsb = endoffset_fsb - startoffset_fsb;

	/*
	 * Allocate file space until done or until there is an error
	 */
	while (allocatesize_fsb && !error) {
		xfs_fileoff_t	s, e;
		bool		cleared_space = false;

		/*
		 * Determine space reservations for data/realtime.
		 */
		if (unlikely(extsz)) {
			s = startoffset_fsb;
			do_div(s, extsz);
			s *= extsz;
			e = startoffset_fsb + allocatesize_fsb;
			div_u64_rem(startoffset_fsb, extsz, &temp);
			if (temp)
				e += temp;
			div_u64_rem(e, extsz, &temp);
			if (temp)
				e += extsz - temp;
		} else {
			s = 0;
			e = allocatesize_fsb;
		}

		/*
		 * The transaction reservation is limited to a 32-bit block
		 * count, hence we need to limit the number of blocks we are
		 * trying to reserve to avoid an overflow. We can't allocate
		 * more than @nimaps extents, and an extent is limited on disk
		 * to MAXEXTLEN (21 bits), so use that to enforce the limit.
		 */
		resblks = min_t(xfs_fileoff_t, (e - s), (MAXEXTLEN * nimaps));
		if (unlikely(rt)) {
			resrtextents = qblocks = resblks;
			resrtextents /= mp->m_sb.sb_rextsize;
			resblks = XFS_DIOSTRAT_SPACE_RES(mp, 0);
			quota_flag = XFS_QMOPT_RES_RTBLKS;
		} else {
			resrtextents = 0;
			resblks = qblocks = XFS_DIOSTRAT_SPACE_RES(mp, resblks);
			quota_flag = XFS_QMOPT_RES_REGBLKS;
		}

		xfs_alloc_reclaim_inactive_space(mp, rt, allocatesize_fsb);

		/*
		 * Allocate and setup the transaction.
		 */
retry:
		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, resblks,
				resrtextents, 0, &tp);
		/*
		 * We weren't able to reserve enough space to handle fallocate.
		 * Flush any disk space that was being held in the hopes of
		 * speeding up the filesystem.  We hold the IOLOCK so we cannot
		 * do a synchronous scan.
		 */
		if (error == -ENOSPC && !cleared_space) {
			cleared_space = true;
			error = xfs_inode_free_blocks(ip->i_mount, false);
			if (!error)
				goto retry;
		}
		/*
		 * Check for running out of space
		 */
		if (error) {
			/*
			 * Free the transaction structure.
			 */
			ASSERT(error == -ENOSPC || XFS_FORCED_SHUTDOWN(mp));
			break;
		}
		xfs_ilock(ip, XFS_ILOCK_EXCL);
		error = xfs_trans_reserve_quota_nblks(tp, ip, qblocks,
						      0, quota_flag);
		/*
		 * We weren't able to reserve enough quota to handle fallocate.
		 * Flush any disk space that was being held in the hopes of
		 * speeding up the filesystem.  We hold the IOLOCK so we cannot
		 * do a synchronous scan.
		 */
		if ((error == -ENOSPC || error == -EDQUOT) && !cleared_space) {
			xfs_trans_cancel(tp);
			xfs_iunlock(ip, XFS_ILOCK_EXCL);
			cleared_space = xfs_inode_free_quota_blocks(ip, false);
			if (cleared_space)
				goto retry;
			return error;
		}
		if (error)
			goto error1;

		xfs_trans_ijoin(tp, ip, 0);

		error = xfs_bmapi_write(tp, ip, startoffset_fsb,
					allocatesize_fsb, alloc_type, 0, imapp,
					&nimaps);
		if (error)
			goto error0;

		/*
		 * Complete the transaction
		 */
		error = xfs_trans_commit(tp);
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
		if (error)
			break;

		allocated_fsb = imapp->br_blockcount;

		if (nimaps == 0) {
			error = -ENOSPC;
			break;
		}

		startoffset_fsb += allocated_fsb;
		allocatesize_fsb -= allocated_fsb;
	}

	return error;

error0:	/* unlock inode, unreserve quota blocks, cancel trans */
	xfs_trans_unreserve_quota_nblks(tp, ip, (long)qblocks, 0, quota_flag);

error1:	/* Just cancel transaction */
	xfs_trans_cancel(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}

static int
xfs_unmap_extent(
	struct xfs_inode	*ip,
	xfs_fileoff_t		startoffset_fsb,
	xfs_filblks_t		len_fsb,
	int			*done)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	uint			resblks = XFS_DIOSTRAT_SPACE_RES(mp, 0);
	int			error;

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, resblks, 0, 0, &tp);
	if (error) {
		ASSERT(error == -ENOSPC || XFS_FORCED_SHUTDOWN(mp));
		return error;
	}

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	error = xfs_trans_reserve_quota(tp, mp, ip->i_udquot, ip->i_gdquot,
			ip->i_pdquot, resblks, 0, XFS_QMOPT_RES_REGBLKS);
	if (error)
		goto out_trans_cancel;

	xfs_trans_ijoin(tp, ip, 0);

	error = xfs_bunmapi(tp, ip, startoffset_fsb, len_fsb, 0, 2, done);
	if (error)
		goto out_trans_cancel;

	error = xfs_trans_commit(tp);
out_unlock:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;

out_trans_cancel:
	xfs_trans_cancel(tp);
	goto out_unlock;
}

/* Caller must first wait for the completion of any pending DIOs if required. */
int
xfs_flush_unmap_range(
	struct xfs_inode	*ip,
	xfs_off_t		offset,
	xfs_off_t		len)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct inode		*inode = VFS_I(ip);
	xfs_off_t		rounding, start, end;
	int			error;

	rounding = max_t(xfs_off_t, 1 << mp->m_sb.sb_blocklog, PAGE_SIZE);
	start = round_down(offset, rounding);
	end = round_up(offset + len, rounding) - 1;

	error = filemap_write_and_wait_range(inode->i_mapping, start, end);
	if (error)
		return error;
	truncate_pagecache_range(inode, start, end);
	return 0;
}

int
xfs_free_file_space(
	struct xfs_inode	*ip,
	xfs_off_t		offset,
	xfs_off_t		len)
{
	struct xfs_mount	*mp = ip->i_mount;
	xfs_fileoff_t		startoffset_fsb;
	xfs_fileoff_t		endoffset_fsb;
	int			done = 0, error;

	trace_xfs_free_file_space(ip);

	error = xfs_qm_dqattach(ip);
	if (error)
		return error;

	if (len <= 0)	/* if nothing being freed */
		return 0;

	startoffset_fsb = XFS_B_TO_FSB(mp, offset);
	endoffset_fsb = XFS_B_TO_FSBT(mp, offset + len);

	/*
	 * Need to zero the stuff we're not freeing, on disk.
	 */
	if (endoffset_fsb > startoffset_fsb) {
		while (!done) {
			error = xfs_unmap_extent(ip, startoffset_fsb,
					endoffset_fsb - startoffset_fsb, &done);
			if (error)
				return error;
		}
	}

	/*
	 * Now that we've unmap all full blocks we'll have to zero out any
	 * partial block at the beginning and/or end.  iomap_zero_range is smart
	 * enough to skip any holes, including those we just created, but we
	 * must take care not to zero beyond EOF and enlarge i_size.
	 */
	if (offset >= XFS_ISIZE(ip))
		return 0;
	if (offset + len > XFS_ISIZE(ip))
		len = XFS_ISIZE(ip) - offset;
	error = iomap_zero_range(VFS_I(ip), offset, len, NULL,
			&xfs_buffered_write_iomap_ops);
	if (error)
		return error;

	/*
	 * If we zeroed right up to EOF and EOF straddles a page boundary we
	 * must make sure that the post-EOF area is also zeroed because the
	 * page could be mmap'd and iomap_zero_range doesn't do that for us.
	 * Writeback of the eof page will do this, albeit clumsily.
	 */
	if (offset + len >= XFS_ISIZE(ip) && offset_in_page(offset + len) > 0) {
		error = filemap_write_and_wait_range(VFS_I(ip)->i_mapping,
				round_down(offset + len, PAGE_SIZE), LLONG_MAX);
	}

	return error;
}

static int
xfs_prepare_shift(
	struct xfs_inode	*ip,
	loff_t			offset)
{
	struct xfs_mount	*mp = ip->i_mount;
	int			error;

	/*
	 * Trim eofblocks to avoid shifting uninitialized post-eof preallocation
	 * into the accessible region of the file.
	 */
	if (xfs_can_free_eofblocks(ip, true)) {
		error = xfs_free_eofblocks(ip);
		if (error)
			return error;
	}

	/*
	 * Shift operations must stabilize the start block offset boundary along
	 * with the full range of the operation. If we don't, a COW writeback
	 * completion could race with an insert, front merge with the start
	 * extent (after split) during the shift and corrupt the file. Start
	 * with the block just prior to the start to stabilize the boundary.
	 */
	offset = round_down(offset, 1 << mp->m_sb.sb_blocklog);
	if (offset)
		offset -= (1 << mp->m_sb.sb_blocklog);

	/*
	 * Writeback and invalidate cache for the remainder of the file as we're
	 * about to shift down every extent from offset to EOF.
	 */
	error = xfs_flush_unmap_range(ip, offset, XFS_ISIZE(ip));
	if (error)
		return error;

	/*
	 * Clean out anything hanging around in the cow fork now that
	 * we've flushed all the dirty data out to disk to avoid having
	 * CoW extents at the wrong offsets.
	 */
	if (xfs_inode_has_cow_data(ip)) {
		error = xfs_reflink_cancel_cow_range(ip, offset, NULLFILEOFF,
				true);
		if (error)
			return error;
	}

	return 0;
}

/*
 * xfs_collapse_file_space()
 *	This routine frees disk space and shift extent for the given file.
 *	The first thing we do is to free data blocks in the specified range
 *	by calling xfs_free_file_space(). It would also sync dirty data
 *	and invalidate page cache over the region on which collapse range
 *	is working. And Shift extent records to the left to cover a hole.
 * RETURNS:
 *	0 on success
 *	errno on error
 *
 */
int
xfs_collapse_file_space(
	struct xfs_inode	*ip,
	xfs_off_t		offset,
	xfs_off_t		len)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	int			error;
	xfs_fileoff_t		next_fsb = XFS_B_TO_FSB(mp, offset + len);
	xfs_fileoff_t		shift_fsb = XFS_B_TO_FSB(mp, len);
	bool			done = false;

	ASSERT(xfs_isilocked(ip, XFS_IOLOCK_EXCL));
	ASSERT(xfs_isilocked(ip, XFS_MMAPLOCK_EXCL));

	trace_xfs_collapse_file_space(ip);

	error = xfs_free_file_space(ip, offset, len);
	if (error)
		return error;

	error = xfs_prepare_shift(ip, offset);
	if (error)
		return error;

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, 0, 0, 0, &tp);
	if (error)
		return error;

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, ip, 0);

	while (!done) {
		error = xfs_bmap_collapse_extents(tp, ip, &next_fsb, shift_fsb,
				&done);
		if (error)
			goto out_trans_cancel;
		if (done)
			break;

		/* finish any deferred frees and roll the transaction */
		error = xfs_defer_finish(&tp);
		if (error)
			goto out_trans_cancel;
	}

	error = xfs_trans_commit(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;

out_trans_cancel:
	xfs_trans_cancel(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}

/*
 * xfs_insert_file_space()
 *	This routine create hole space by shifting extents for the given file.
 *	The first thing we do is to sync dirty data and invalidate page cache
 *	over the region on which insert range is working. And split an extent
 *	to two extents at given offset by calling xfs_bmap_split_extent.
 *	And shift all extent records which are laying between [offset,
 *	last allocated extent] to the right to reserve hole range.
 * RETURNS:
 *	0 on success
 *	errno on error
 */
int
xfs_insert_file_space(
	struct xfs_inode	*ip,
	loff_t			offset,
	loff_t			len)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	int			error;
	xfs_fileoff_t		stop_fsb = XFS_B_TO_FSB(mp, offset);
	xfs_fileoff_t		next_fsb = NULLFSBLOCK;
	xfs_fileoff_t		shift_fsb = XFS_B_TO_FSB(mp, len);
	bool			done = false;

	ASSERT(xfs_isilocked(ip, XFS_IOLOCK_EXCL));
	ASSERT(xfs_isilocked(ip, XFS_MMAPLOCK_EXCL));

	trace_xfs_insert_file_space(ip);

	error = xfs_bmap_can_insert_extents(ip, stop_fsb, shift_fsb);
	if (error)
		return error;

	error = xfs_prepare_shift(ip, offset);
	if (error)
		return error;

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write,
			XFS_DIOSTRAT_SPACE_RES(mp, 0), 0, 0, &tp);
	if (error)
		return error;

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, ip, 0);

	/*
	 * The extent shifting code works on extent granularity. So, if stop_fsb
	 * is not the starting block of extent, we need to split the extent at
	 * stop_fsb.
	 */
	error = xfs_bmap_split_extent(tp, ip, stop_fsb);
	if (error)
		goto out_trans_cancel;

	do {
		error = xfs_trans_roll_inode(&tp, ip);
		if (error)
			goto out_trans_cancel;

		error = xfs_bmap_insert_extents(tp, ip, &next_fsb, shift_fsb,
				&done, stop_fsb);
		if (error)
			goto out_trans_cancel;
	} while (!done);

	error = xfs_trans_commit(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;

out_trans_cancel:
	xfs_trans_cancel(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}
