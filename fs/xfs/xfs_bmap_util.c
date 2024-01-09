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
#include "xfs_rtbitmap.h"
#include "xfs_swapext.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_rtrefcount_btree.h"
#include "xfs_health.h"
#include "xfs_alloc_btree.h"
#include "xfs_rmap.h"
#include "xfs_ag.h"

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
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_btree_cur	*cur;
	xfs_extlen_t		btblocks = 0;
	int			error;

	*nextents = 0;
	*count = 0;

	if (!ifp)
		return 0;

	switch (ifp->if_format) {
	case XFS_DINODE_FMT_BTREE:
		error = xfs_iread_extents(tp, ip, whichfork);
		if (error)
			return error;

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

		fallthrough;
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
		 * Take the flush completion as being a point-in-time snapshot
		 * where there are no delalloc extents, and if any new ones
		 * have been created racily, just skip them as being 'after'
		 * the flush and so don't get reported.
		 */
		if (!(bmv->bmv_iflags & BMV_IF_DELALLOC))
			return 0;

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

	xfs_ilock(ip, XFS_IOLOCK_SHARED);
	switch (whichfork) {
	case XFS_ATTR_FORK:
		lock = xfs_ilock_attr_map_shared(ip);
		if (!xfs_inode_has_attr_fork(ip))
			goto out_unlock_ilock;

		max_len = 1LL << 32;
		break;
	case XFS_COW_FORK:
		lock = XFS_ILOCK_SHARED;
		xfs_ilock(ip, lock);

		/* No CoW fork? Just return */
		if (!xfs_ifork_ptr(ip, whichfork))
			goto out_unlock_ilock;

		if (xfs_get_cowextsz_hint(ip))
			max_len = mp->m_super->s_maxbytes;
		else
			max_len = XFS_ISIZE(ip);
		break;
	case XFS_DATA_FORK:
		if (!(iflags & BMV_IF_DELALLOC) &&
		    (ip->i_delayed_blks || XFS_ISIZE(ip) > ip->i_disk_size)) {
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
		    (ip->i_diflags &
		     (XFS_DIFLAG_PREALLOC | XFS_DIFLAG_APPEND)))
			max_len = mp->m_super->s_maxbytes;
		else
			max_len = XFS_ISIZE(ip);

		lock = xfs_ilock_data_map_shared(ip);
		break;
	}

	ifp = xfs_ifork_ptr(ip, whichfork);

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

	error = xfs_iread_extents(NULL, ip, whichfork);
	if (error)
		goto out_unlock_ilock;

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

			if (bmv->bmv_entries > 0)
				out[bmv->bmv_entries - 1].bmv_oflags |=
								BMV_OF_LAST;

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
	xfs_off_t		start_byte,
	xfs_off_t		end_byte)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_ifork	*ifp = &ip->i_df;
	xfs_fileoff_t		start_fsb = XFS_B_TO_FSBT(mp, start_byte);
	xfs_fileoff_t		end_fsb = XFS_B_TO_FSB(mp, end_byte);
	struct xfs_bmbt_irec	got, del;
	struct xfs_iext_cursor	icur;
	int			error = 0;

	ASSERT(!xfs_need_iread_extents(ifp));

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	if (!xfs_iext_lookup_extent_before(ip, ifp, &end_fsb, &icur, &got))
		goto out_unlock;

	while (got.br_startoff + got.br_blockcount > start_fsb) {
		del = got;
		xfs_trim_extent(&del, start_fsb, end_fsb - start_fsb);

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
xfs_can_free_eofblocks(
	struct xfs_inode	*ip,
	bool			force)
{
	struct xfs_bmbt_irec	imap;
	struct xfs_mount	*mp = ip->i_mount;
	xfs_fileoff_t		end_fsb;
	xfs_fileoff_t		last_fsb;
	int			nimaps = 1;
	int			error;

	/*
	 * Caller must either hold the exclusive io lock; or be inactivating
	 * the inode, which guarantees there are no other users of the inode.
	 */
	ASSERT(xfs_isilocked(ip, XFS_IOLOCK_EXCL) ||
	       (VFS_I(ip)->i_state & I_FREEING));

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
	if (xfs_need_iread_extents(&ip->i_df))
		return false;

	/*
	 * Do not free extent size hints, real preallocated or append-only files
	 * unless the file has delalloc blocks and we are forced to remove
	 * them.
	 */
	if (xfs_get_extsz_hint(ip) ||
	    (ip->i_diflags & (XFS_DIFLAG_PREALLOC | XFS_DIFLAG_APPEND))) {
		if (!force || ip->i_delayed_blks == 0)
			return false;
	}

	/*
	 * Do not try to free post-EOF blocks if EOF is beyond the end of the
	 * range supported by the page cache, because the truncation will loop
	 * forever.
	 */
	end_fsb = XFS_B_TO_FSB(mp, (xfs_ufsize_t)XFS_ISIZE(ip));
	if (xfs_inode_has_bigallocunit(ip))
		end_fsb = xfs_rtb_roundup_rtx(mp, end_fsb);
	last_fsb = XFS_B_TO_FSB(mp, mp->m_super->s_maxbytes);
	if (last_fsb <= end_fsb)
		return false;

	/*
	 * Look up the mapping for the first block past EOF.  If we can't find
	 * it, there's nothing to free.
	 */
	xfs_ilock(ip, XFS_ILOCK_SHARED);
	error = xfs_bmapi_read(ip, end_fsb, last_fsb - end_fsb, &imap, &nimaps,
			0);
	xfs_iunlock(ip, XFS_ILOCK_SHARED);
	if (error || nimaps == 0)
		return false;

	/*
	 * If there's a real mapping there or there are delayed allocation
	 * reservations, then we have post-EOF blocks to try to free.
	 */
	return imap.br_startblock != HOLESTARTBLOCK || ip->i_delayed_blks;
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
	int			error;

	/* Attach the dquots to the inode up front. */
	error = xfs_qm_dqattach(ip);
	if (error)
		return error;

	/* Wait on dio to ensure i_size has settled. */
	inode_dio_wait(VFS_I(ip));

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0, &tp);
	if (error) {
		ASSERT(xfs_is_shutdown(mp));
		return error;
	}

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, ip, 0);

	/*
	 * Do not update the on-disk file size.  If we update the on-disk file
	 * size and then the system crashes before the contents of the file are
	 * flushed to disk then the files may be full of holes (ie NULL files
	 * bug).
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

int
xfs_alloc_file_space(
	struct xfs_inode	*ip,
	xfs_off_t		offset,
	xfs_off_t		len)
{
	xfs_mount_t		*mp = ip->i_mount;
	xfs_off_t		count;
	xfs_filblks_t		allocatesize_fsb;
	xfs_extlen_t		extsz, temp;
	xfs_fileoff_t		startoffset_fsb;
	xfs_fileoff_t		endoffset_fsb;
	int			rt;
	xfs_trans_t		*tp;
	xfs_bmbt_irec_t		imaps[1], *imapp;
	int			error;

	trace_xfs_alloc_file_space(ip, offset, len);

	if (xfs_is_shutdown(mp))
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
	startoffset_fsb	= XFS_B_TO_FSBT(mp, offset);
	endoffset_fsb = XFS_B_TO_FSB(mp, offset + count);
	allocatesize_fsb = endoffset_fsb - startoffset_fsb;

	/*
	 * Allocate file space until done or until there is an error
	 */
	while (allocatesize_fsb && !error) {
		xfs_fileoff_t	s, e;
		unsigned int	dblocks, rblocks, resblks;
		int		nimaps = 1;

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
		 * to XFS_BMBT_MAX_EXTLEN (21 bits), so use that to enforce the
		 * limit.
		 */
		resblks = min_t(xfs_fileoff_t, (e - s),
				(XFS_MAX_BMBT_EXTLEN * nimaps));
		if (unlikely(rt)) {
			dblocks = XFS_DIOSTRAT_SPACE_RES(mp, 0);
			rblocks = resblks;
		} else {
			dblocks = XFS_DIOSTRAT_SPACE_RES(mp, resblks);
			rblocks = 0;
		}

		error = xfs_trans_alloc_inode(ip, &M_RES(mp)->tr_write,
				dblocks, rblocks, false, &tp);
		if (error)
			break;

		error = xfs_iext_count_may_overflow(ip, XFS_DATA_FORK,
				XFS_IEXT_ADD_NOSPLIT_CNT);
		if (error == -EFBIG)
			error = xfs_iext_count_upgrade(tp, ip,
					XFS_IEXT_ADD_NOSPLIT_CNT);
		if (error)
			goto error;

		error = xfs_bmapi_write(tp, ip, startoffset_fsb,
				allocatesize_fsb, XFS_BMAPI_PREALLOC, 0, imapp,
				&nimaps);
		if (error)
			goto error;

		ip->i_diflags |= XFS_DIFLAG_PREALLOC;
		xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);

		error = xfs_trans_commit(tp);
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
		if (error)
			break;

		/*
		 * If the allocator cannot find a single free extent large
		 * enough to cover the start block of the requested range,
		 * xfs_bmapi_write will return 0 but leave *nimaps set to 0.
		 *
		 * In that case we simply need to keep looping with the same
		 * startoffset_fsb so that one of the following allocations
		 * will eventually reach the requested range.
		 */
		if (nimaps) {
			startoffset_fsb += imapp->br_blockcount;
			allocatesize_fsb -= imapp->br_blockcount;
		}
	}

	return error;

error:
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

	error = xfs_trans_alloc_inode(ip, &M_RES(mp)->tr_write, resblks, 0,
			false, &tp);
	if (error)
		return error;

	error = xfs_iext_count_may_overflow(ip, XFS_DATA_FORK,
			XFS_IEXT_PUNCH_HOLE_CNT);
	if (error == -EFBIG)
		error = xfs_iext_count_upgrade(tp, ip, XFS_IEXT_PUNCH_HOLE_CNT);
	if (error)
		goto out_trans_cancel;

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

	rounding = max_t(xfs_off_t, mp->m_sb.sb_blocksize, PAGE_SIZE);
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

	trace_xfs_free_file_space(ip, offset, len);

	error = xfs_qm_dqattach(ip);
	if (error)
		return error;

	if (len <= 0)	/* if nothing being freed */
		return 0;

	startoffset_fsb = XFS_B_TO_FSB(mp, offset);
	endoffset_fsb = XFS_B_TO_FSBT(mp, offset + len);

	/* We can only free complete realtime extents. */
	if (xfs_inode_has_bigallocunit(ip)) {
		startoffset_fsb = xfs_rtb_roundup_rtx(mp, startoffset_fsb);
		endoffset_fsb = xfs_rtb_rounddown_rtx(mp, endoffset_fsb);
	}

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
	 * partial block at the beginning and/or end.  xfs_zero_range is smart
	 * enough to skip any holes, including those we just created, but we
	 * must take care not to zero beyond EOF and enlarge i_size.
	 */
	if (offset >= XFS_ISIZE(ip))
		return 0;
	if (offset + len > XFS_ISIZE(ip))
		len = XFS_ISIZE(ip) - offset;
	error = xfs_zero_range(ip, offset, len, NULL);
	if (error)
		return error;

	/*
	 * If we zeroed right up to EOF and EOF straddles a page boundary we
	 * must make sure that the post-EOF area is also zeroed because the
	 * page could be mmap'd and xfs_zero_range doesn't do that for us.
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
	offset = round_down(offset, mp->m_sb.sb_blocksize);
	if (offset)
		offset -= mp->m_sb.sb_blocksize;

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

	trace_xfs_collapse_file_space(ip, offset, len);

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

	trace_xfs_insert_file_space(ip, offset, len);

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

	error = xfs_iext_count_may_overflow(ip, XFS_DATA_FORK,
			XFS_IEXT_PUNCH_HOLE_CNT);
	if (error == -EFBIG)
		error = xfs_iext_count_upgrade(tp, ip, XFS_IEXT_PUNCH_HOLE_CNT);
	if (error)
		goto out_trans_cancel;

	/*
	 * The extent shifting code works on extent granularity. So, if stop_fsb
	 * is not the starting block of extent, we need to split the extent at
	 * stop_fsb.
	 */
	error = xfs_bmap_split_extent(tp, ip, stop_fsb);
	if (error)
		goto out_trans_cancel;

	do {
		error = xfs_defer_finish(&tp);
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

/*
 * Decide if this is an unwritten extent that isn't aligned to an allocation
 * unit boundary.
 *
 * If it is, shorten the mapping to the end of the allocation unit so that
 * we're ready to convert all the mappings for this allocation unit to a zeroed
 * written extent.  If not, return false.
 */
static inline bool
xfs_want_convert_bigalloc_mapping(
	struct xfs_mount	*mp,
	struct xfs_bmbt_irec	*irec)
{
	xfs_fileoff_t		rext_next;
	xfs_extlen_t		modoff, modcnt;

	if (irec->br_state != XFS_EXT_UNWRITTEN)
		return false;

	modoff = xfs_rtb_to_rtxoff(mp, irec->br_startoff);
	if (modoff == 0) {
		xfs_rtbxlen_t	rexts;

		rexts = xfs_rtb_to_rtxrem(mp, irec->br_blockcount, &modcnt);
		if (rexts > 0) {
			/*
			 * Unwritten mapping starts at an rt extent boundary
			 * and is longer than one rt extent.  Round the length
			 * down to the nearest extent but don't select it for
			 * conversion.
			 */
			irec->br_blockcount -= modcnt;
			modcnt = 0;
		}

		/* Unwritten mapping is perfectly aligned, do not convert. */
		if (modcnt == 0)
			return false;
	}

	/*
	 * Unaligned and unwritten; trim to the current rt extent and select it
	 * for conversion.
	 */
	rext_next = (irec->br_startoff - modoff) + mp->m_sb.sb_rextsize;
	xfs_trim_extent(irec, irec->br_startoff, rext_next - irec->br_startoff);
	return true;
}

/*
 * Find an unwritten extent in the given file range, zero it, and convert the
 * mapping to written.  Adjust the scan cursor on the way out.
 */
STATIC int
xfs_convert_bigalloc_mapping(
	struct xfs_inode	*ip,
	xfs_fileoff_t		*offp,
	xfs_fileoff_t		endoff)
{
	struct xfs_bmbt_irec	irec;
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	unsigned int		resblks;
	int			nmap;
	int			error;

	resblks = XFS_DIOSTRAT_SPACE_RES(mp, 1);
	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, resblks, 0, 0, &tp);
	if (error)
		return error;

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, ip, 0);

	/*
	 * Read the mapping.  If we find an unwritten extent that isn't aligned
	 * to an allocation unit...
	 */
retry:
	nmap = 1;
	error = xfs_bmapi_read(ip, *offp, endoff - *offp, &irec, &nmap, 0);
	if (error)
		goto out_cancel;
	ASSERT(nmap == 1);
	ASSERT(irec.br_startoff == *offp);
	if (!xfs_want_convert_bigalloc_mapping(mp, &irec)) {
		*offp = irec.br_startoff + irec.br_blockcount;
		if (*offp >= endoff)
			goto out_cancel;
		goto retry;
	}

	/*
	 * ...then write zeroes to the space and change the mapping state to
	 * written.  This consolidates the mappings for this allocation unit.
	 */
	nmap = 1;
	error = xfs_bmapi_write(tp, ip, irec.br_startoff, irec.br_blockcount,
			XFS_BMAPI_CONVERT | XFS_BMAPI_ZERO, 0, &irec, &nmap);
	if (error)
		goto out_cancel;
	error = xfs_trans_commit(tp);
	if (error)
		goto out_unlock;

	xfs_iunlock(ip, XFS_ILOCK_EXCL);

	/*
	 * If an unwritten mapping was returned, something is very wrong.
	 * If no mapping was returned, then bmapi_write thought it performed
	 * a short allocation, which should be impossible since we previously
	 * queried the mapping and haven't cycled locks since then.  Either
	 * way, fail the operation.
	 */
	if (nmap == 0 || irec.br_state != XFS_EXT_NORM) {
		ASSERT(nmap != 0);
		ASSERT(irec.br_state == XFS_EXT_NORM);
		return -EIO;
	}

	/* Advance the cursor to the end of the mapping returned. */
	*offp = irec.br_startoff + irec.br_blockcount;
	return 0;

out_cancel:
	xfs_trans_cancel(tp);
out_unlock:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}

/*
 * Prepare a file with multi-fsblock allocation units for a remapping.
 *
 * File allocation units (AU) must be fully mapped to the data fork.  If the
 * space in an AU have not been fully written, there can be multiple extent
 * mappings (e.g. mixed written and unwritten blocks) to the AU.  If the log
 * does not have a means to ensure that all remappings for a given AU will be
 * completed even if the fs goes down, we must maintain the above constraint in
 * another way.
 *
 * Convert the unwritten parts of an AU to written by writing zeroes to the
 * storage and flipping the mapping.  Once this completes, there will be a
 * single mapping for the entire AU, and we can proceed with the remapping
 * operation.
 *
 * Callers must ensure that there are no dirty pages in the given range.
 */
int
xfs_convert_bigalloc_file_space(
	struct xfs_inode	*ip,
	loff_t			pos,
	uint64_t		len)
{
	struct xfs_mount	*mp = ip->i_mount;
	xfs_fileoff_t		off;
	xfs_fileoff_t		endoff;
	int			error;

	if (!xfs_inode_has_bigallocunit(ip))
		return 0;

	off = xfs_rtb_rounddown_rtx(mp, XFS_B_TO_FSBT(mp, pos));
	endoff = xfs_rtb_roundup_rtx(mp, XFS_B_TO_FSB(mp, pos + len));

	trace_xfs_convert_bigalloc_file_space(ip, pos, len);

	while (off < endoff) {
		if (fatal_signal_pending(current))
			return -EINTR;

		error = xfs_convert_bigalloc_mapping(ip, &off, endoff);
		if (error)
			return error;
	}

	return 0;
}

/*
 * Reserve space and quota to this transaction to map in as much free space
 * as we can.  Callers should set @len to the amount of space desired; this
 * function will shorten that quantity if it can't get space.
 */
STATIC int
xfs_map_free_reserve_more(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	xfs_extlen_t		*len)
{
	struct xfs_mount	*mp = ip->i_mount;
	unsigned int		dblocks;
	unsigned int		rblocks;
	unsigned int		min_len;
	bool			isrt = XFS_IS_REALTIME_INODE(ip);
	int			error;

	if (*len > XFS_MAX_BMBT_EXTLEN)
		*len = XFS_MAX_BMBT_EXTLEN;
	min_len = isrt ? mp->m_sb.sb_rextsize : 1;

again:
	if (isrt) {
		dblocks = XFS_EXTENTADD_SPACE_RES(mp, XFS_DATA_FORK);
		rblocks = *len;
	} else {
		dblocks = XFS_DIOSTRAT_SPACE_RES(mp, *len);
		rblocks = 0;
	}
	error = xfs_trans_reserve_more_inode(tp, ip, dblocks, rblocks, false);
	if (error == -ENOSPC && *len > min_len) {
		*len >>= 1;
		goto again;
	}
	if (error) {
		trace_xfs_map_free_reserve_more_fail(ip, error, _RET_IP_);
		return error;
	}

	return 0;
}

/* Find a free extent in this AG and map it into the file. */
STATIC int
xfs_map_free_extent(
	struct xfs_inode	*ip,
	struct xfs_perag	*pag,
	xfs_agblock_t		*cursor,
	xfs_agblock_t		end_agbno,
	xfs_agblock_t		*last_enospc_agbno)
{
	struct xfs_bmbt_irec	irec;
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	xfs_off_t		endpos;
	xfs_fsblock_t		fsbno;
	xfs_extlen_t		free_len, map_len;
	int			error;

	if (fatal_signal_pending(current))
		return -EINTR;

	error = xfs_trans_alloc_inode(ip, &M_RES(mp)->tr_write, 0, 0, false,
			&tp);
	if (error)
		return error;

	error = xfs_alloc_find_freesp(tp, pag, cursor, end_agbno, &free_len);
	if (error)
		goto out_cancel;

	/* Bail out if the cursor is beyond what we asked for. */
	if (*cursor >= end_agbno)
		goto out_cancel;

	error = xfs_map_free_reserve_more(tp, ip, &free_len);
	if (error)
		goto out_cancel;

	fsbno = XFS_AGB_TO_FSB(mp, pag->pag_agno, *cursor);
	map_len = free_len;
	do {
		error = xfs_bmapi_freesp(tp, ip, fsbno, map_len, &irec);
		if (error == -EAGAIN) {
			/* Failed to map space but were told to try again. */
			error = xfs_trans_commit(tp);
			goto out;
		}
		if (error != -ENOSPC)
			break;
		/*
		 * If we can't get the space, try asking for successively less
		 * space in case we're bumping up against per-AG metadata
		 * reservation limits.
		 */
		map_len >>= 1;
	} while (map_len > 0);
	if (error == -ENOSPC) {
		if (*last_enospc_agbno != *cursor) {
			/*
			 * However, backing off on the size of the mapping
			 * request might not work if an AGFL fixup allocated
			 * the block at *cursor.  The first time this happens,
			 * remember that we ran out of space here, and try
			 * again.
			 */
			*last_enospc_agbno = *cursor;
		} else {
			/*
			 * If we hit this a second time on the same extent,
			 * then it's likely that we're bumping up against
			 * per-AG space reservation limits.  Skip to the next
			 * extent.
			 */
			*cursor += free_len;
		}
		error = 0;
		goto out_cancel;
	}
	if (error)
		goto out_cancel;

	/* Update isize if needed. */
	endpos = XFS_FSB_TO_B(mp, irec.br_startoff + irec.br_blockcount);
	if (endpos > i_size_read(VFS_I(ip))) {
		i_size_write(VFS_I(ip), endpos);
		ip->i_disk_size = endpos;
		xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	}

	error = xfs_trans_commit(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	if (error)
		return error;

	*cursor += irec.br_blockcount;
	return 0;
out_cancel:
	xfs_trans_cancel(tp);
out:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}

/*
 * Allocate all free physical space between off and len and map it to this
 * regular non-realtime file.
 */
int
xfs_map_free_space(
	struct xfs_inode	*ip,
	xfs_off_t		off,
	xfs_off_t		len)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_perag	*pag = NULL;
	xfs_daddr_t		off_daddr = BTOBB(off);
	xfs_daddr_t		end_daddr = BTOBBT(off + len);
	xfs_fsblock_t		off_fsb = XFS_DADDR_TO_FSB(mp, off_daddr);
	xfs_fsblock_t		end_fsb = XFS_DADDR_TO_FSB(mp, end_daddr);
	xfs_agnumber_t		off_agno = XFS_FSB_TO_AGNO(mp, off_fsb);
	xfs_agnumber_t		end_agno = XFS_FSB_TO_AGNO(mp, end_fsb);
	xfs_agnumber_t		agno;
	int			error = 0;

	trace_xfs_map_free_space(ip, off, len);

	agno = off_agno;
	for_each_perag_range(mp, agno, end_agno, pag) {
		xfs_agblock_t	off_agbno = 0;
		xfs_agblock_t	end_agbno;
		xfs_agblock_t	last_enospc_agbno = NULLAGBLOCK;

		end_agbno = xfs_ag_block_count(mp, pag->pag_agno);

		if (pag->pag_agno == off_agno)
			off_agbno = XFS_FSB_TO_AGBNO(mp, off_fsb);
		if (pag->pag_agno == end_agno)
			end_agbno = XFS_FSB_TO_AGBNO(mp, end_fsb);

		while (off_agbno < end_agbno) {
			error = xfs_map_free_extent(ip, pag, &off_agbno,
					end_agbno, &last_enospc_agbno);
			if (error)
				goto out;
		}
	}

out:
	if (pag)
		xfs_perag_rele(pag);
	if (error == -ENOSPC)
		return 0;
	return error;
}

#ifdef CONFIG_XFS_RT
STATIC int
xfs_map_free_rt_extent(
	struct xfs_inode	*ip,
	xfs_rtxnum_t		*cursor,
	xfs_rtxnum_t		end_rtx)
{
	struct xfs_bmbt_irec	irec;
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	xfs_off_t		endpos;
	xfs_rtblock_t		rtbno;
	xfs_rtxlen_t		len_rtx;
	xfs_extlen_t		len;
	int			error;

	if (fatal_signal_pending(current))
		return -EINTR;

	error = xfs_trans_alloc_inode(ip, &M_RES(mp)->tr_write, 0, 0, false,
			&tp);
	if (error)
		return error;

	xfs_rtbitmap_lock(tp, mp);
	error = xfs_rtallocate_find_freesp(tp, cursor, end_rtx, &len_rtx);
	if (error)
		goto out_cancel;

	/*
	 * If off_rtx is beyond the end of the rt device or is past what the
	 * user asked for, bail out.
	 */
	if (*cursor >= end_rtx)
		goto out_cancel;

	len = xfs_rtx_to_rtb(mp, len_rtx);
	error = xfs_map_free_reserve_more(tp, ip, &len);
	if (error)
		goto out_cancel;

	rtbno = xfs_rtx_to_rtb(mp, *cursor);
	error = xfs_bmapi_freesp(tp, ip, rtbno, len, &irec);
	if (error)
		goto out_cancel;

	/* Update isize if needed. */
	endpos = XFS_FSB_TO_B(mp, irec.br_startoff + irec.br_blockcount);
	if (endpos > i_size_read(VFS_I(ip))) {
		i_size_write(VFS_I(ip), endpos);
		ip->i_disk_size = endpos;
		xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	}

	error = xfs_trans_commit(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	if (error)
		return error;

	ASSERT(xfs_rtb_to_rtxoff(mp, irec.br_blockcount) == 0);
	*cursor += xfs_rtb_to_rtx(mp, irec.br_blockcount);
	return 0;
out_cancel:
	xfs_trans_cancel(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}

/*
 * Allocate all free physical space between off and len and map it to this
 * regular non-realtime file.
 */
int
xfs_map_free_rt_space(
	struct xfs_inode	*ip,
	xfs_off_t		off,
	xfs_off_t		len)
{
	struct xfs_mount	*mp = ip->i_mount;
	xfs_rtblock_t		off_rtb = XFS_B_TO_FSB(mp, off);
	xfs_rtblock_t		end_rtb = XFS_B_TO_FSBT(mp, off + len);
	xfs_rtxnum_t		off_rtx;
	xfs_rtxnum_t		end_rtx;
	int			error = 0;

	/* Compute rt extents from the input parameters. */
	off_rtx = xfs_rtb_to_rtxup(mp, off_rtb);
	end_rtx = xfs_rtb_to_rtx(mp, end_rtb);

	if (off_rtx >= mp->m_sb.sb_rextents)
		return 0;
	if (end_rtx >= mp->m_sb.sb_rextents)
		end_rtx = mp->m_sb.sb_rextents - 1;

	trace_xfs_map_free_rt_space(ip, off, len);

	while (off_rtx < end_rtx) {
		error = xfs_map_free_rt_extent(ip, &off_rtx, end_rtx);
		if (error)
			break;
	}

	if (error == -ENOSPC)
		return 0;
	return error;
}
#endif
