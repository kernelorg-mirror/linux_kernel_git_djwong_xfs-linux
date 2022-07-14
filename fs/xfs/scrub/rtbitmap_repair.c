// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_btree.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_rtalloc.h"
#include "xfs_inode.h"
#include "xfs_bit.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_rmap.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_swapext.h"
#include "xfs_rtbitmap.h"
#include "xfs_rtgroup.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/xfile.h"
#include "scrub/tempfile.h"
#include "scrub/tempswap.h"
#include "scrub/reap.h"

struct xrep_rgbmp {
	struct xfs_scrub	*sc;

	/* file offset inside the rtbitmap where we start swapping */
	xfs_fileoff_t		group_rbmoff;

	/* number of rtbitmap blocks for this group */
	xfs_filblks_t		group_rbmlen;

	/* The next rtgroup block we expect to see during our rtrmapbt walk. */
	xfs_rgblock_t		next_rgbno;
};

/*
 * Compute the byte offset of the xfs_rtword_t corresponding to the given rt
 * extent's location in the bitmap.
 */
static inline loff_t
rtword_off(
	xfs_rtblock_t	rt_ext)
{
	return (rt_ext >> XFS_NBWORDLOG) * sizeof(xfs_rtword_t);
}

/* Perform a logical OR operation on an rtword in the incore bitmap. */
static int
xrep_rgbitmap_or(
	struct xrep_rgbmp	*rb,
	xfs_rtblock_t		rt_ext,
	xfs_rtword_t		mask)
{
	loff_t			pos = rtword_off(rt_ext);
	xfs_rtword_t		word;
	int			error;

	error = xfile_obj_load(rb->sc->xfile, &word, sizeof(word), pos);
	if (error)
		return error;

	trace_xrep_rgbitmap_or(rb->sc->mp, rt_ext, pos, mask, word);

	word |= mask;
	return xfile_obj_store(rb->sc->xfile, &word, sizeof(word), pos);
}

/*
 * Mark as free every rt extent between the next rt block we expected to see
 * in the rtrmap records and the given rt block.
 */
STATIC int
xrep_rgbitmap_mark_free(
	struct xrep_rgbmp	*rb,
	xfs_rgblock_t		rgbno)
{
	struct xfs_mount	*mp = rb->sc->mp;
	struct xfs_rtgroup	*rtg = rb->sc->sr.rtg;
	xfs_rtblock_t		rtbno;
	xfs_rtxnum_t		startrtx;
	xfs_rtxnum_t		nextrtx;
	loff_t			pos, endpos;
	unsigned int		bit;
	xfs_extlen_t		mod;
	xfs_rtword_t		mask;
	int			error;

	if (!xfs_verify_rgbext(rtg, rb->next_rgbno, rgbno - rb->next_rgbno))
		return -EFSCORRUPTED;

	/*
	 * Convert rt blocks to rt extents  The block range we find must be
	 * aligned to an rtextent boundary on both ends.
	 */
	rtbno = xfs_rgbno_to_rtb(mp, rtg->rtg_rgno, rb->next_rgbno);
	startrtx = xfs_rtb_to_rtx(mp, rtbno, &mod);
	if (mod)
		return -EFSCORRUPTED;

	rtbno = xfs_rgbno_to_rtb(mp, rtg->rtg_rgno, rgbno - 1);
	nextrtx = xfs_rtb_to_rtx(mp, rtbno, &mod) + 1;
	if (mod != mp->m_sb.sb_rextsize - 1)
		return -EFSCORRUPTED;

	trace_xrep_rgbitmap_record_free(mp, startrtx, nextrtx - 1);

	/* Set bits as needed to round startrtx up to the nearest word. */
	bit = startrtx & (XFS_NBWORD - 1);
	if (bit) {
		xfs_rtblock_t	len = nextrtx - startrtx;
		unsigned int	lastbit;

		lastbit = XFS_RTMIN(bit + len, XFS_NBWORD);
		mask = (((xfs_rtword_t)1 << (lastbit - bit)) - 1) << bit;

		error = xrep_rgbitmap_or(rb, startrtx, mask);
		if (error || lastbit - bit == len)
			return error;
		startrtx += XFS_NBWORD - bit;
	}

	/* Set bits as needed to round nextrtx down to the nearest word. */
	bit = nextrtx & (XFS_NBWORD - 1);
	if (bit) {
		mask = ((xfs_rtword_t)1 << bit) - 1;

		error = xrep_rgbitmap_or(rb, nextrtx, mask);
		if (error || startrtx + bit == nextrtx)
			return error;
		nextrtx -= bit;
	}

	trace_xrep_rgbitmap_record_free_bulk(mp, startrtx, nextrtx - 1);

	/* Set all the bytes in between, up to a whole fs block at once. */
	pos = startrtx >> XFS_NBBYLOG;
	endpos = nextrtx >> XFS_NBBYLOG;

	while (pos < endpos) {
		loff_t	rem;
		size_t	count = min_t(loff_t, endpos - pos,
				      mp->m_sb.sb_blocksize);

		/* Try to get us aligned to an even blocksize. */
		rem = pos & (mp->m_sb.sb_blocksize - 1);
		if (rem)
			count = min_t(loff_t, count,
				      mp->m_sb.sb_blocksize - rem);

		/* Write all ones to this part of the xfile. */
		error = xfile_obj_store(rb->sc->xfile, rb->sc->buf, count, pos);
		if (error)
			return error;
		pos += count;
	}

	return 0;
}

/* Set up to repair the realtime bitmap for this group. */
int
xrep_setup_rgbitmap(
	struct xfs_scrub	*sc,
	unsigned int		*resblks)
{
	struct xfs_mount	*mp = sc->mp;
	unsigned long long	blocks = 0;
	loff_t			bmp_bytes;
	size_t			bufsize = mp->m_sb.sb_blocksize;
	int			error;

	error = xrep_tempfile_create(sc, S_IFREG);
	if (error)
		return error;

	/* Create an xfile to hold our reconstructed bitmap. */
	bmp_bytes = XFS_FSB_TO_B(mp, mp->m_sb.sb_rbmblocks);
	error = xfile_create(sc->mp, "rtbitmap", bmp_bytes, &sc->xfile);
	if (error)
		return error;

	bufsize = max(bufsize, sizeof(struct xrep_tempswap));

	/*
	 * Allocate a memory buffer for faster creation of new bitmap
	 * blocks.
	 */
	sc->buf = kvmalloc(bufsize, XCHK_GFP_FLAGS);
	if (!sc->buf)
		return -ENOMEM;

	/*
	 * Reserve enough blocks to write out a completely new bitmap file,
	 * plus twice as many blocks as we would need if we can only allocate
	 * one block per data fork mapping.  This should cover the
	 * preallocation of the temporary file and swapping the extent
	 * mappings.
	 *
	 * We cannot use xfs_swapext_estimate because we have not yet
	 * constructed the replacement bitmap and therefore do not know how
	 * many extents it will use.  By the time we do, we will have a dirty
	 * transaction (which we cannot drop because we cannot drop the
	 * rtbitmap ILOCK) and cannot ask for more reservation.
	 */
	blocks = mp->m_sb.sb_rbmblocks;
	blocks += xfs_bmbt_calc_size(mp, blocks) * 2;
	if (blocks > UINT_MAX)
		return -EOPNOTSUPP;

	*resblks += blocks;

	/*
	 * Grab support for atomic extent swapping before we allocate any
	 * transactions or grab ILOCKs.
	 */
	return xrep_tempswap_grab_log_assist(sc);
}

/* Set free space in the rtbitmap based on rtrmapbt records. */
STATIC int
xrep_rgbitmap_walk_rtrmap(
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec,
	void				*priv)
{
	struct xrep_rgbmp		*rb = priv;
	int				error = 0;

	if (xchk_should_terminate(rb->sc, &error))
		return error;

	if (rb->next_rgbno < rec->rm_startblock) {
		error = xrep_rgbitmap_mark_free(rb, rec->rm_startblock);
		if (error)
			return error;
	}

	rb->next_rgbno = max(rb->next_rgbno,
			rec->rm_startblock + rec->rm_blockcount);
	return 0;
}

/*
 * Walk the rtrmapbt to find all the gaps between records, and mark the gaps
 * in the realtime bitmap that we're computing.
 */
STATIC int
xrep_rgbitmap_find_freespace(
	struct xrep_rgbmp	*rb)
{
	struct xfs_scrub	*sc = rb->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_rtgroup	*rtg = sc->sr.rtg;
	int			error;

	/* Prepare a buffer of ones so that we can accelerate bulk setting. */
	memset(sc->buf, 0xFF, mp->m_sb.sb_blocksize);

	xrep_rtgroup_btcur_init(sc, &sc->sr);
	error = xfs_rmap_query_all(sc->sr.rmap_cur, xrep_rgbitmap_walk_rtrmap,
			rb);
	if (error || rtg->rtg_blockcount == rb->next_rgbno)
		goto out;

	/*
	 * Mark as free every possible rt extent from the last one we saw to
	 * the end of the rt group.
	 */
	error = xrep_rgbitmap_mark_free(rb, rtg->rtg_blockcount);
	if (error)
		goto out;

out:
	xchk_rtgroup_btcur_free(&sc->sr);
	return error;
}

/*
 * Preserve the portions of the rtbitmap block for the start of this rtgroup
 * that map to the previous rtgroup.
 */
static int
xrep_rgbitmap_load_before(
	struct xrep_rgbmp	*rb)
{
	struct xfs_scrub	*sc = rb->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_rtgroup	*rtg = sc->sr.rtg;
	struct xfs_buf		*bp;
	unsigned long long	wordoff;
	xfs_rtblock_t		group_rtbno;
	xfs_rtxnum_t		group_rtx, rbmoff_rtx;
	xfs_rtword_t		ondisk_word;
	xfs_rtword_t		xfile_word;
	xfs_rtword_t		mask;
	unsigned int		wordcnt;
	int			bit;
	int			error;

	/*
	 * Compute the file offset within the rtbitmap block that corresponds
	 * to the start of this group, and decide if we need to read blocks
	 * from the group before this one.
	 */
	group_rtbno = xfs_rgbno_to_rtb(mp, rtg->rtg_rgno, 0);
	group_rtx = xfs_rtb_to_rtxt(mp, group_rtbno);

	rb->group_rbmoff = XFS_BITTOBLOCK(mp, group_rtx);
	rbmoff_rtx = XFS_BLOCKTOBIT(mp, rb->group_rbmoff);

	trace_xrep_rgbitmap_load(rtg, rb->group_rbmoff, rbmoff_rtx,
			group_rtx - 1);

	if (rbmoff_rtx == group_rtx)
		return 0;

	error = xfs_rtbuf_get(mp, sc->tp, rb->group_rbmoff, 0, &bp);
	if (error) {
		/*
		 * Reading the existing rbmblock failed, and we must deal with
		 * the part of the rtbitmap block that corresponds to the
		 * previous group.  The most conservative option is to fill
		 * that part of the bitmap with zeroes so that it won't get
		 * allocated.  The xfile contains zeroes already, so we can
		 * return.
		 */
		return 0;
	}

	/*
	 * Copy full rtbitmap words into memory from the beginning of the
	 * ondisk block until we get to the word that corresponds to the start
	 * of this group.
	 */
	wordoff = (rbmoff_rtx >> XFS_NBWORDLOG);
	wordcnt = XFS_BITTOWORD(mp, group_rtx - rbmoff_rtx);
	if (wordcnt > 0) {
		error = xfile_obj_store(sc->xfile, bp->b_addr,
				wordcnt * sizeof(xfs_rtword_t),
				wordoff * sizeof(xfs_rtword_t));
		if (error)
			goto out_rele;

		trace_xrep_rgbitmap_load_words(mp, rb->group_rbmoff, wordoff,
				wordcnt);
	}

	/*
	 * Compute the bit position of the first rtextent of this group.  If
	 * the bit position is zero, we don't have to RMW a partial word and
	 * move to the next step.
	 */
	bit = group_rtx & (XFS_NBWORD - 1);
	if (bit == 0)
		goto out_rele;

	/*
	 * Create a mask of the bits that we want to load from disk.  These
	 * bits track space in a different rtgroup, which is why we must
	 * preserve them even as we replace parts of the bitmap.
	 */
	mask = ~((((xfs_rtword_t)1 << (XFS_NBWORD - bit)) - 1) << bit);

	wordoff += wordcnt;
	error = xfile_obj_load(sc->xfile, &xfile_word, sizeof(xfs_rtword_t),
			wordoff * sizeof(xfs_rtword_t));
	if (error)
		goto out_rele;
	ondisk_word = *((xfs_rtword_t *)bp->b_addr + wordcnt);

	trace_xrep_rgbitmap_load_word(mp, wordoff, bit, ondisk_word,
			xfile_word, mask);

	xfile_word &= ~mask;
	xfile_word |= (ondisk_word & mask);

	error = xfile_obj_store(sc->xfile, &xfile_word, sizeof(xfs_rtword_t),
			wordoff * sizeof(xfs_rtword_t));
	if (error)
		goto out_rele;

out_rele:
	xfs_trans_brelse(sc->tp, bp);
	return error;
}

/*
 * Preserve the portions of the rtbitmap block for the end of this rtgroup
 * that map to the next rtgroup.
 */
static int
xrep_rgbitmap_load_after(
	struct xrep_rgbmp	*rb)
{
	struct xfs_scrub	*sc = rb->sc;
	struct xfs_mount	*mp = rb->sc->mp;
	struct xfs_rtgroup	*rtg = rb->sc->sr.rtg;
	struct xfs_buf		*bp;
	unsigned long long	wordoff;
	xfs_rtblock_t		last_rtbno;
	xfs_rtxnum_t		last_group_rtx, last_rbmblock_rtx;
	xfs_fileoff_t		last_group_rbmoff;
	xfs_rtword_t		ondisk_word;
	xfs_rtword_t		xfile_word;
	xfs_rtword_t		mask;
	unsigned int		wordcnt;
	int			bit;
	int			error;

	last_rtbno = xfs_rgbno_to_rtb(mp, rtg->rtg_rgno,
					rtg->rtg_blockcount - 1);
	last_group_rtx = xfs_rtb_to_rtxt(mp, last_rtbno);

	last_group_rbmoff = XFS_BITTOBLOCK(mp, last_group_rtx);
	rb->group_rbmlen = last_group_rbmoff - rb->group_rbmoff + 1;
	last_rbmblock_rtx = XFS_BLOCKTOBIT(mp, last_group_rbmoff + 1) - 1;

	trace_xrep_rgbitmap_load(rtg, last_group_rbmoff, last_group_rtx + 1,
			last_rbmblock_rtx);

	if (last_rbmblock_rtx == last_group_rtx ||
	    rtg->rtg_rgno == mp->m_sb.sb_rgcount - 1)
		return 0;

	error = xfs_rtbuf_get(mp, sc->tp, last_group_rbmoff, 0, &bp);
	if (error) {
		/*
		 * Reading the existing rbmblock failed, and we must deal with
		 * the part of the rtbitmap block that corresponds to the
		 * previous group.  The most conservative option is to fill
		 * that part of the bitmap with zeroes so that it won't get
		 * allocated.  The xfile contains zeroes already, so we can
		 * return.
		 */
		return 0;
	}

	/*
	 * Compute the bit position of the first rtextent of the next group.
	 * If the bit position is zero, we don't have to RMW a partial word
	 * and move to the next step.
	 */
	wordoff = last_group_rtx >> XFS_NBWORDLOG;
	bit = (last_group_rtx + 1) & (XFS_NBWORD - 1);
	if (bit == 0)
		goto copy_words;

	/*
	 * Create a mask of the bits that we want to load from disk.  These
	 * bits track space in a different rtgroup, which is why we must
	 * preserve them even as we replace parts of the bitmap.
	 */
	mask = (((xfs_rtword_t)1 << (XFS_NBWORD - bit)) - 1) << bit;

	error = xfile_obj_load(sc->xfile, &xfile_word, sizeof(xfs_rtword_t),
			wordoff * sizeof(xfs_rtword_t));
	if (error)
		goto out_rele;
	ondisk_word = *((xfs_rtword_t *)bp->b_addr +
					XFS_BITTOWORD(mp, last_group_rtx));

	trace_xrep_rgbitmap_load_word(mp, wordoff, bit, ondisk_word,
			xfile_word, mask);

	xfile_word &= ~mask;
	xfile_word |= (ondisk_word & mask);

	error = xfile_obj_store(sc->xfile, &xfile_word, sizeof(xfs_rtword_t),
			wordoff * sizeof(xfs_rtword_t));
	if (error)
		goto out_rele;

copy_words:
	/* Copy as many full words as we can. */
	wordoff++;
	wordcnt = XFS_BITTOWORD(mp, last_rbmblock_rtx - last_group_rtx + 1);
	if (wordcnt > 0) {
		xfs_rtword_t	*p = bp->b_addr;

		p += mp->m_blockwsize - wordcnt;
		error = xfile_obj_store(sc->xfile, p,
				wordcnt * sizeof(xfs_rtword_t),
				wordoff * sizeof(xfs_rtword_t));
		if (error)
			goto out_rele;

		trace_xrep_rgbitmap_load_words(mp, last_group_rbmoff, wordoff,
				wordcnt);
	}

out_rele:
	xfs_trans_brelse(sc->tp, bp);
	return error;
}

/* Repair the realtime bitmap for this rt group. */
int
xrep_rgbitmap(
	struct xfs_scrub	*sc)
{
	struct xrep_rgbmp	rb = {
		.sc		= sc,
		.next_rgbno	= 0,
	};
	struct xrep_tempswap	*ti = NULL;
	int			error;

	/*
	 * We require the realtime rmapbt (and atomic file updates) to rebuild
	 * anything.
	 */
	if (!xfs_has_rtrmapbt(sc->mp))
		return -EOPNOTSUPP;

	error = xrep_rgbitmap_load_before(&rb);
	if (error)
		return error;
	error = xrep_rgbitmap_load_after(&rb);
	if (error)
		return error;

	/*
	 * Generate the new rtbitmap data.  We don't need the rtbmp information
	 * once this call is finished.
	 */
	error = xrep_rgbitmap_find_freespace(&rb);
	if (error)
		return error;

	/*
	 * Try to take ILOCK_EXCL of the temporary file.  We had better be the
	 * only ones holding onto this inode, but we can't block while holding
	 * the rtbitmap file's ILOCK_EXCL.
	 */
	while (!xrep_tempfile_ilock_nowait(sc)) {
		if (xchk_should_terminate(sc, &error))
			return error;
		delay(1);
	}

	/*
	 * Make sure we have space allocated for the part of the bitmap
	 * file that corresponds to this group.
	 */
	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	xfs_trans_ijoin(sc->tp, sc->tempip, 0);
	error = xrep_tempfile_prealloc(sc, rb.group_rbmoff, rb.group_rbmlen);
	if (error)
		return error;

	/* Last chance to abort before we start committing fixes. */
	if (xchk_should_terminate(sc, &error))
		return error;

	/* Copy the bitmap file that we generated. */
	error = xrep_tempfile_copyin_xfile(sc, &xfs_rtbuf_ops,
			XFS_BLFT_RTBITMAP_BUF, rb.group_rbmoff,
			rb.group_rbmlen);
	if (error)
		return error;
	error = xrep_tempfile_set_isize(sc,
			XFS_FSB_TO_B(sc->mp, sc->mp->m_sb.sb_rbmblocks));
	if (error)
		return error;

	/*
	 * Now swap the extents.  We're done with the temporary buffer, so
	 * we can reuse it for the tempfile swapext information.
	 */
	ti = sc->buf;
	error = xrep_tempswap_trans_reserve(sc, XFS_DATA_FORK, rb.group_rbmoff,
			rb.group_rbmlen, ti);
	if (error)
		return error;

	error = xrep_tempswap_contents(sc, ti);
	if (error)
		return error;
	ti = NULL;

	/* Free the old bitmap blocks if they are free. */
	return xrep_reap_ifork(sc, sc->tempip, XFS_DATA_FORK);
}

/* Set up to repair the realtime bitmap file metadata. */
int
xrep_setup_rtbitmap(
	struct xfs_scrub	*sc,
	unsigned int		*resblks)
{
	struct xfs_mount	*mp = sc->mp;
	unsigned long long	blocks = 0;

	/*
	 * Reserve enough blocks to write out a completely new bmbt for the
	 * bitmap file.
	 */
	blocks = xfs_bmbt_calc_size(mp, mp->m_sb.sb_rbmblocks);
	if (blocks > UINT_MAX)
		return -EOPNOTSUPP;

	*resblks += blocks;
	return 0;
}

/* Repair the realtime bitmap file metadata. */
int
xrep_rtbitmap(
	struct xfs_scrub	*sc)
{
	/*
	 * The only thing we know how to fix right now is problems with the
	 * inode or its fork data.
	 */
	return xrep_metadata_inode_forks(sc);
}
