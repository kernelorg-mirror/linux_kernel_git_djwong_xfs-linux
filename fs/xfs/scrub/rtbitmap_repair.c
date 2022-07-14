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

/*
 * We use an xfile to construct new bitmap blocks for the portion of the
 * rtbitmap file that we're replacing.  Whereas the ondisk bitmap must be
 * accessed through the buffer cache, the xfile bitmap supports direct
 * word-level accesses.  Therefore, we create a small abstraction for linear
 * access.
 */
typedef unsigned long long xrep_wordoff_t;
typedef unsigned int xrep_wordcnt_t;

struct xrep_rgbmp {
	struct xfs_scrub	*sc;

	/* file offset inside the rtbitmap where we start swapping */
	xfs_fileoff_t		group_rbmoff;

	/* number of rtbitmap blocks for this group */
	xfs_filblks_t		group_rbmlen;

	/* The next rtgroup block we expect to see during our rtrmapbt walk. */
	xfs_rgblock_t		next_rgbno;

	/* rtword position of xfile as we write buffers to disk. */
	xrep_wordoff_t		prep_wordoff;
};

/* Mask to round an rtx down to the nearest bitmap word. */
#define XREP_RTBMP_WORDMASK	((1ULL << XFS_NBWORDLOG) - 1)

/* Set up to repair the realtime bitmap for this group. */
int
xrep_setup_rgbitmap(
	struct xfs_scrub	*sc,
	unsigned int		*resblks)
{
	struct xfs_mount	*mp = sc->mp;
	unsigned long long	blocks = 0;
	unsigned long long	rtbmp_words;
	size_t			bufsize = mp->m_sb.sb_blocksize;
	int			error;

	error = xrep_tempfile_create(sc, S_IFREG);
	if (error)
		return error;

	/* Create an xfile to hold our reconstructed bitmap. */
	rtbmp_words = xfs_rtbitmap_wordcount(mp, mp->m_sb.sb_rextents);
	error = xfile_create(sc->mp, "rtbitmap", rtbmp_words << XFS_WORDLOG,
			&sc->xfile);
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

static inline xrep_wordoff_t
rtx_to_wordoff(
	struct xfs_mount	*mp,
	xfs_rtxnum_t		rtx)
{
	return rtx >> XFS_NBWORDLOG;
}

static inline xrep_wordcnt_t
rtxlen_to_wordcnt(
	xfs_rtxlen_t	rtxlen)
{
	return rtxlen >> XFS_NBWORDLOG;
}

/* Helper functions to record rtwords in an xfile. */

static inline int
xfbmp_load(
	struct xrep_rgbmp	*rb,
	xrep_wordoff_t		wordoff,
	xfs_rtword_t		*word)
{
	union xfs_rtword_ondisk	urk;
	int			error;

	error = xfile_obj_load(rb->sc->xfile, &urk,
			sizeof(union xfs_rtword_ondisk),
			wordoff << XFS_WORDLOG);
	if (error)
		return error;

	*word = xfs_rtbitmap_getword(rb->sc->mp, &urk);
	return 0;
}

static inline int
xfbmp_store(
	struct xrep_rgbmp	*rb,
	xrep_wordoff_t		wordoff,
	const xfs_rtword_t	word)
{
	union xfs_rtword_ondisk	urk;

	xfs_rtbitmap_setword(rb->sc->mp, &urk, word);
	return xfile_obj_store(rb->sc->xfile, &urk,
			sizeof(union xfs_rtword_ondisk),
			wordoff << XFS_WORDLOG);
}

static inline int
xfbmp_copyin(
	struct xrep_rgbmp	*rb,
	xrep_wordoff_t		wordoff,
	const union xfs_rtword_ondisk	*word,
	xrep_wordcnt_t		nr_words)
{
	return xfile_obj_store(rb->sc->xfile, word, nr_words << XFS_WORDLOG,
			wordoff << XFS_WORDLOG);
}

static inline int
xfbmp_copyout(
	struct xrep_rgbmp	*rb,
	xrep_wordoff_t		wordoff,
	union xfs_rtword_ondisk	*word,
	xrep_wordcnt_t		nr_words)
{
	return xfile_obj_load(rb->sc->xfile, word, nr_words << XFS_WORDLOG,
			wordoff << XFS_WORDLOG);
}

/*
 * Preserve the portions of the rtbitmap block for the start of this rtgroup
 * that map to the previous rtgroup.
 */
STATIC int
xrep_rgbitmap_load_before(
	struct xrep_rgbmp	*rb)
{
	struct xfs_scrub	*sc = rb->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_rtgroup	*rtg = sc->sr.rtg;
	struct xfs_buf		*bp;
	xrep_wordoff_t		wordoff;
	xfs_rtblock_t		group_rtbno;
	xfs_rtxnum_t		group_rtx, rbmoff_rtx;
	xfs_rtword_t		ondisk_word;
	xfs_rtword_t		xfile_word;
	xfs_rtword_t		mask;
	xrep_wordcnt_t		wordcnt;
	int			bit;
	int			error;

	/*
	 * Compute the file offset within the rtbitmap block that corresponds
	 * to the start of this group, and decide if we need to read blocks
	 * from the group before this one.
	 */
	group_rtbno = xfs_rgbno_to_rtb(mp, rtg->rtg_rgno, 0);
	group_rtx = xfs_rtb_to_rtxt(mp, group_rtbno);

	rb->group_rbmoff = xfs_rtx_to_rbmblock(mp, group_rtx);
	rbmoff_rtx = xfs_rbmblock_to_rtx(mp, rb->group_rbmoff);
	rb->prep_wordoff = rtx_to_wordoff(mp, rbmoff_rtx);

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
	wordoff = rtx_to_wordoff(mp, rbmoff_rtx);
	wordcnt = rtxlen_to_wordcnt(group_rtx - rbmoff_rtx);
	if (wordcnt > 0) {
		union xfs_rtword_ondisk	*p;

		p = xfs_rbmblock_wordptr(bp, 0);
		error = xfbmp_copyin(rb, wordoff, p, wordcnt);
		if (error)
			goto out_rele;

		trace_xrep_rgbitmap_load_words(mp, rb->group_rbmoff, wordoff,
				wordcnt);
		wordoff += wordcnt;
	}

	/*
	 * Compute the bit position of the first rtextent of this group.  If
	 * the bit position is zero, we don't have to RMW a partial word and
	 * move to the next step.
	 */
	bit = group_rtx & XREP_RTBMP_WORDMASK;
	if (bit == 0)
		goto out_rele;

	/*
	 * Create a mask of the bits that we want to load from disk.  These
	 * bits track space in a different rtgroup, which is why we must
	 * preserve them even as we replace parts of the bitmap.
	 */
	mask = ~((((xfs_rtword_t)1 << (XFS_NBWORD - bit)) - 1) << bit);

	error = xfbmp_load(rb, wordoff, &xfile_word);
	if (error)
		goto out_rele;
	ondisk_word = xfs_rtbitmap_getword(mp,
			xfs_rbmblock_wordptr(bp, wordcnt));

	trace_xrep_rgbitmap_load_word(mp, wordoff, bit, ondisk_word,
			xfile_word, mask);

	xfile_word &= ~mask;
	xfile_word |= (ondisk_word & mask);

	error = xfbmp_store(rb, wordoff, xfile_word);
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
STATIC int
xrep_rgbitmap_load_after(
	struct xrep_rgbmp	*rb)
{
	struct xfs_scrub	*sc = rb->sc;
	struct xfs_mount	*mp = rb->sc->mp;
	struct xfs_rtgroup	*rtg = rb->sc->sr.rtg;
	struct xfs_buf		*bp;
	xrep_wordoff_t		wordoff;
	xfs_rtblock_t		last_rtbno;
	xfs_rtxnum_t		last_group_rtx, last_rbmblock_rtx;
	xfs_fileoff_t		last_group_rbmoff;
	xfs_rtword_t		ondisk_word;
	xfs_rtword_t		xfile_word;
	xfs_rtword_t		mask;
	xrep_wordcnt_t		wordcnt;
	unsigned int		last_group_word;
	int			bit;
	int			error;

	last_rtbno = xfs_rgbno_to_rtb(mp, rtg->rtg_rgno,
					rtg->rtg_blockcount - 1);
	last_group_rtx = xfs_rtb_to_rtxt(mp, last_rtbno);

	last_group_rbmoff = xfs_rtx_to_rbmblock(mp, last_group_rtx);
	rb->group_rbmlen = last_group_rbmoff - rb->group_rbmoff + 1;
	last_rbmblock_rtx = xfs_rbmblock_to_rtx(mp, last_group_rbmoff + 1) - 1;

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
	wordoff = rtx_to_wordoff(mp, last_group_rtx);
	bit = (last_group_rtx + 1) & XREP_RTBMP_WORDMASK;
	if (bit == 0)
		goto copy_words;

	/*
	 * Create a mask of the bits that we want to load from disk.  These
	 * bits track space in a different rtgroup, which is why we must
	 * preserve them even as we replace parts of the bitmap.
	 */
	mask = (((xfs_rtword_t)1 << (XFS_NBWORD - bit)) - 1) << bit;

	error = xfbmp_load(rb, wordoff, &xfile_word);
	if (error)
		goto out_rele;
	last_group_word = xfs_rtx_to_rbmword(mp, last_group_rtx);
	ondisk_word = xfs_rtbitmap_getword(mp,
			xfs_rbmblock_wordptr(bp, last_group_word));

	trace_xrep_rgbitmap_load_word(mp, wordoff, bit, ondisk_word,
			xfile_word, mask);

	xfile_word &= ~mask;
	xfile_word |= (ondisk_word & mask);

	error = xfbmp_store(rb, wordoff, xfile_word);
	if (error)
		goto out_rele;

copy_words:
	/* Copy as many full words as we can. */
	wordoff++;
	wordcnt = rtxlen_to_wordcnt(last_rbmblock_rtx - last_group_rtx);
	if (wordcnt > 0) {
		union xfs_rtword_ondisk	*p;

		p = xfs_rbmblock_wordptr(bp, mp->m_blockwsize - wordcnt);
		error = xfbmp_copyin(rb, wordoff, p, wordcnt);
		if (error)
			goto out_rele;

		trace_xrep_rgbitmap_load_words(mp, last_group_rbmoff, wordoff,
				wordcnt);
	}

out_rele:
	xfs_trans_brelse(sc->tp, bp);
	return error;
}

/* Perform a logical OR operation on an rtword in the incore bitmap. */
static int
xrep_rgbitmap_or(
	struct xrep_rgbmp	*rb,
	xrep_wordoff_t		wordoff,
	xfs_rtword_t		mask)
{
	xfs_rtword_t		word;
	int			error;

	error = xfbmp_load(rb, wordoff, &word);
	if (error)
		return error;

	trace_xrep_rgbitmap_or(rb->sc->mp, wordoff, mask, word);

	return xfbmp_store(rb, wordoff, word | mask);
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
	xrep_wordoff_t		wordoff, nextwordoff;
	unsigned int		bit;
	unsigned int		bufwsize;
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
	bit = startrtx & XREP_RTBMP_WORDMASK;
	if (bit) {
		xfs_rtblock_t	len = nextrtx - startrtx;
		unsigned int	lastbit;

		lastbit = XFS_RTMIN(bit + len, XFS_NBWORD);
		mask = (((xfs_rtword_t)1 << (lastbit - bit)) - 1) << bit;

		error = xrep_rgbitmap_or(rb, rtx_to_wordoff(mp, startrtx), mask);
		if (error || lastbit - bit == len)
			return error;
		startrtx += XFS_NBWORD - bit;
	}

	/* Set bits as needed to round nextrtx down to the nearest word. */
	bit = nextrtx & XREP_RTBMP_WORDMASK;
	if (bit) {
		mask = ((xfs_rtword_t)1 << bit) - 1;

		error = xrep_rgbitmap_or(rb, rtx_to_wordoff(mp, nextrtx), mask);
		if (error || startrtx + bit == nextrtx)
			return error;
		nextrtx -= bit;
	}

	trace_xrep_rgbitmap_record_free_bulk(mp, startrtx, nextrtx - 1);

	/* Set all the words in between, up to a whole fs block at once. */
	wordoff = rtx_to_wordoff(mp, startrtx);
	nextwordoff = rtx_to_wordoff(mp, nextrtx);
	bufwsize = mp->m_sb.sb_blocksize >> XFS_WORDLOG;

	while (wordoff < nextwordoff) {
		xrep_wordoff_t	rem;
		xrep_wordcnt_t	wordcnt;

		wordcnt = min_t(xrep_wordcnt_t, nextwordoff - wordoff,
				bufwsize);

		/*
		 * Try to keep us aligned to sc->buf to reduce the number of
		 * xfile writes.
		 */
		rem = wordoff & (bufwsize - 1);
		if (rem)
			wordcnt = min_t(xrep_wordcnt_t, wordcnt,
					bufwsize - rem);

		error = xfbmp_copyin(rb, wordoff, rb->sc->buf, wordcnt);
		if (error)
			return error;

		wordoff += wordcnt;
	}

	return 0;
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
	if (error)
		goto out;

	/*
	 * Mark as free every possible rt extent from the last one we saw to
	 * the end of the rt group.
	 */
	if (rb->next_rgbno < rtg->rtg_blockcount) {
		error = xrep_rgbitmap_mark_free(rb, rtg->rtg_blockcount);
		if (error)
			goto out;
	}

out:
	xchk_rtgroup_btcur_free(&sc->sr);
	return error;
}

static int
xrep_rgbitmap_prep_buf(
	struct xfs_scrub	*sc,
	struct xfs_buf		*bp,
	void			*data)
{
	struct xrep_rgbmp	*rb = data;
	struct xfs_mount	*mp = sc->mp;
	int			error;

	error = xfbmp_copyout(rb, rb->prep_wordoff,
			xfs_rbmblock_wordptr(bp, 0), mp->m_blockwsize);
	if (error)
		return error;

	if (xfs_has_rtgroups(sc->mp)) {
		struct xfs_rtbuf_blkinfo	*hdr = bp->b_addr;

		hdr->rt_magic = cpu_to_be32(XFS_RTBITMAP_MAGIC);
		hdr->rt_owner = cpu_to_be64(sc->ip->i_ino);
		hdr->rt_blkno = cpu_to_be64(xfs_buf_daddr(bp));
		hdr->rt_lsn = 0;
		uuid_copy(&hdr->rt_uuid, &sc->mp->m_sb.sb_meta_uuid);
		bp->b_ops = &xfs_rtbitmap_buf_ops;
	} else {
		bp->b_ops = &xfs_rtbuf_ops;
	}

	rb->prep_wordoff += mp->m_blockwsize;
	xfs_trans_buf_set_type(sc->tp, bp, XFS_BLFT_RTBITMAP_BUF);
	return 0;
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

	/*
	 * If the start or end of this rt group happens to be in the middle of
	 * an rtbitmap block, try to read in the parts of the bitmap that are
	 * from some other group.
	 */
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
	error = xrep_tempfile_copyin(sc, rb.group_rbmoff, rb.group_rbmlen,
			xrep_rgbitmap_prep_buf, &rb);
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
