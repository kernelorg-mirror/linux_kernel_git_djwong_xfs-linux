// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020-2023 Oracle.  All Rights Reserved.
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
#include "xfs_refcount.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/xfile.h"
#include "scrub/tempfile.h"
#include "scrub/tempswap.h"
#include "scrub/reap.h"
#include "scrub/rtbitmap.h"

/* rt bitmap content repairs */

/* Set up to repair the realtime bitmap for this group. */
int
xrep_setup_rgbitmap(
	struct xfs_scrub	*sc,
	struct xchk_rgbitmap	*rgb)
{
	struct xfs_mount	*mp = sc->mp;
	char			*descr;
	unsigned long long	blocks = 0;
	unsigned long long	rtbmp_words;
	int			error;

	error = xrep_tempfile_create(sc, S_IFREG);
	if (error)
		return error;

	/* Create an xfile to hold our reconstructed bitmap. */
	rtbmp_words = xfs_rtbitmap_wordcount(mp, mp->m_sb.sb_rextents);
	descr = xchk_xfile_rtgroup_descr(sc, "bitmap file");
	error = xfile_create(descr, rtbmp_words << XFS_WORDLOG, &sc->xfile);
	kfree(descr);
	if (error)
		return error;

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

	rgb->rtb.resblks += blocks;

	/*
	 * Grab support for atomic extent swapping before we allocate any
	 * transactions or grab ILOCKs.
	 */
	error = xrep_tempswap_grab_log_assist(sc);
	if (error)
		return error;

	/*
	 * We must hold rbmip with ILOCK_EXCL to use the extent swap at the end
	 * of the repair function.  Change the desired rtglock flags.
	 */
	rgb->rtglock_flags &= ~XFS_RTGLOCK_BITMAP_SHARED;
	rgb->rtglock_flags |= XFS_RTGLOCK_BITMAP;
	return 0;
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
	struct xchk_rgbitmap	*rgb,
	xrep_wordoff_t		wordoff,
	xfs_rtword_t		*word)
{
	union xfs_rtword_raw	urk;
	int			error;

	ASSERT(xfs_has_rtgroups(rgb->sc->mp));

	error = xfile_obj_load(rgb->sc->xfile, &urk,
			sizeof(union xfs_rtword_raw),
			wordoff << XFS_WORDLOG);
	if (error)
		return error;

	*word = le32_to_cpu(urk.rtg);
	return 0;
}

static inline int
xfbmp_store(
	struct xchk_rgbitmap	*rgb,
	xrep_wordoff_t		wordoff,
	const xfs_rtword_t	word)
{
	union xfs_rtword_raw	urk;

	ASSERT(xfs_has_rtgroups(rgb->sc->mp));

	urk.rtg = cpu_to_le32(word);
	return xfile_obj_store(rgb->sc->xfile, &urk,
			sizeof(union xfs_rtword_raw),
			wordoff << XFS_WORDLOG);
}

static inline int
xfbmp_copyin(
	struct xchk_rgbitmap	*rgb,
	xrep_wordoff_t		wordoff,
	const union xfs_rtword_raw	*word,
	xrep_wordcnt_t		nr_words)
{
	return xfile_obj_store(rgb->sc->xfile, word, nr_words << XFS_WORDLOG,
			wordoff << XFS_WORDLOG);
}

static inline int
xfbmp_copyout(
	struct xchk_rgbitmap	*rgb,
	xrep_wordoff_t		wordoff,
	union xfs_rtword_raw	*word,
	xrep_wordcnt_t		nr_words)
{
	return xfile_obj_load(rgb->sc->xfile, word, nr_words << XFS_WORDLOG,
			wordoff << XFS_WORDLOG);
}

/*
 * Preserve the portions of the rtbitmap block for the start of this rtgroup
 * that map to the previous rtgroup.
 */
STATIC int
xrep_rgbitmap_load_before(
	struct xchk_rgbitmap	*rgb)
{
	struct xfs_scrub	*sc = rgb->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_rtgroup	*rtg = sc->sr.rtg;
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
	group_rtx = xfs_rtb_to_rtx(mp, group_rtbno);

	rgb->group_rbmoff = xfs_rtx_to_rbmblock(mp, group_rtx);
	rbmoff_rtx = xfs_rbmblock_to_rtx(mp, rgb->group_rbmoff);
	rgb->prep_wordoff = rtx_to_wordoff(mp, rbmoff_rtx);

	trace_xrep_rgbitmap_load(rtg, rgb->group_rbmoff, rbmoff_rtx,
			group_rtx - 1);

	if (rbmoff_rtx == group_rtx)
		return 0;

	rgb->args.mp = sc->mp;
	rgb->args.tp = sc->tp;
	error = xfs_rtbitmap_read_buf(&rgb->args, rgb->group_rbmoff);
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
		union xfs_rtword_raw	*p;

		p = xfs_rbmblock_wordptr(&rgb->args, 0);
		error = xfbmp_copyin(rgb, wordoff, p, wordcnt);
		if (error)
			goto out_rele;

		trace_xrep_rgbitmap_load_words(mp, rgb->group_rbmoff, wordoff,
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

	error = xfbmp_load(rgb, wordoff, &xfile_word);
	if (error)
		goto out_rele;
	ondisk_word = xfs_rtbitmap_getword(&rgb->args, wordcnt);

	trace_xrep_rgbitmap_load_word(mp, wordoff, bit, ondisk_word,
			xfile_word, mask);

	xfile_word &= ~mask;
	xfile_word |= (ondisk_word & mask);

	error = xfbmp_store(rgb, wordoff, xfile_word);
	if (error)
		goto out_rele;

out_rele:
	xfs_rtbuf_cache_relse(&rgb->args);
	return error;
}

/*
 * Preserve the portions of the rtbitmap block for the end of this rtgroup
 * that map to the next rtgroup.
 */
STATIC int
xrep_rgbitmap_load_after(
	struct xchk_rgbitmap	*rgb)
{
	struct xfs_scrub	*sc = rgb->sc;
	struct xfs_mount	*mp = rgb->sc->mp;
	struct xfs_rtgroup	*rtg = rgb->sc->sr.rtg;
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
	last_group_rtx = xfs_rtb_to_rtx(mp, last_rtbno);

	last_group_rbmoff = xfs_rtx_to_rbmblock(mp, last_group_rtx);
	rgb->group_rbmlen = last_group_rbmoff - rgb->group_rbmoff + 1;
	last_rbmblock_rtx = xfs_rbmblock_to_rtx(mp, last_group_rbmoff + 1) - 1;

	trace_xrep_rgbitmap_load(rtg, last_group_rbmoff, last_group_rtx + 1,
			last_rbmblock_rtx);

	if (last_rbmblock_rtx == last_group_rtx ||
	    rtg->rtg_rgno == mp->m_sb.sb_rgcount - 1)
		return 0;

	rgb->args.mp = sc->mp;
	rgb->args.tp = sc->tp;
	error = xfs_rtbitmap_read_buf(&rgb->args, last_group_rbmoff);
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

	error = xfbmp_load(rgb, wordoff, &xfile_word);
	if (error)
		goto out_rele;
	last_group_word = xfs_rtx_to_rbmword(mp, last_group_rtx);
	ondisk_word = xfs_rtbitmap_getword(&rgb->args, last_group_word);

	trace_xrep_rgbitmap_load_word(mp, wordoff, bit, ondisk_word,
			xfile_word, mask);

	xfile_word &= ~mask;
	xfile_word |= (ondisk_word & mask);

	error = xfbmp_store(rgb, wordoff, xfile_word);
	if (error)
		goto out_rele;

copy_words:
	/* Copy as many full words as we can. */
	wordoff++;
	wordcnt = rtxlen_to_wordcnt(last_rbmblock_rtx - last_group_rtx);
	if (wordcnt > 0) {
		union xfs_rtword_raw	*p;

		p = xfs_rbmblock_wordptr(&rgb->args,
				mp->m_blockwsize - wordcnt);
		error = xfbmp_copyin(rgb, wordoff, p, wordcnt);
		if (error)
			goto out_rele;

		trace_xrep_rgbitmap_load_words(mp, last_group_rbmoff, wordoff,
				wordcnt);
	}

out_rele:
	xfs_rtbuf_cache_relse(&rgb->args);
	return error;
}

/* Perform a logical OR operation on an rtword in the incore bitmap. */
static int
xrep_rgbitmap_or(
	struct xchk_rgbitmap	*rgb,
	xrep_wordoff_t		wordoff,
	xfs_rtword_t		mask)
{
	xfs_rtword_t		word;
	int			error;

	error = xfbmp_load(rgb, wordoff, &word);
	if (error)
		return error;

	trace_xrep_rgbitmap_or(rgb->sc->mp, wordoff, mask, word);

	return xfbmp_store(rgb, wordoff, word | mask);
}

/*
 * Mark as free every rt extent between the next rt block we expected to see
 * in the rtrmap records and the given rt block.
 */
STATIC int
xrep_rgbitmap_mark_free(
	struct xchk_rgbitmap	*rgb,
	xfs_rgblock_t		rgbno)
{
	struct xfs_mount	*mp = rgb->sc->mp;
	struct xchk_rt		*sr =  &rgb->sc->sr;
	struct xfs_rtgroup	*rtg = sr->rtg;
	xfs_rtblock_t		rtbno;
	xfs_rtxnum_t		startrtx;
	xfs_rtxnum_t		nextrtx;
	xrep_wordoff_t		wordoff, nextwordoff;
	unsigned int		bit;
	unsigned int		bufwsize;
	xfs_extlen_t		mod;
	xfs_rtword_t		mask;
	enum xbtree_recpacking	outcome;
	int			error;

	if (!xfs_verify_rgbext(rtg, rgb->next_rgbno, rgbno - rgb->next_rgbno))
		return -EFSCORRUPTED;

	/*
	 * Convert rt blocks to rt extents  The block range we find must be
	 * aligned to an rtextent boundary on both ends.
	 */
	rtbno = xfs_rgbno_to_rtb(mp, rtg->rtg_rgno, rgb->next_rgbno);
	startrtx = xfs_rtb_to_rtxrem(mp, rtbno, &mod);
	if (mod)
		return -EFSCORRUPTED;

	rtbno = xfs_rgbno_to_rtb(mp, rtg->rtg_rgno, rgbno - 1);
	nextrtx = xfs_rtb_to_rtxrem(mp, rtbno, &mod) + 1;
	if (mod != mp->m_sb.sb_rextsize - 1)
		return -EFSCORRUPTED;

	/* Must not be shared or CoW staging. */
	if (sr->refc_cur) {
		error = xfs_refcount_has_records(sr->refc_cur,
				XFS_REFC_DOMAIN_SHARED, rgb->next_rgbno,
				rgbno - rgb->next_rgbno, &outcome);
		if (error)
			return error;
		if (outcome != XBTREE_RECPACKING_EMPTY)
			return -EFSCORRUPTED;

		error = xfs_refcount_has_records(sr->refc_cur,
				XFS_REFC_DOMAIN_COW, rgb->next_rgbno,
				rgbno - rgb->next_rgbno, &outcome);
		if (error)
			return error;
		if (outcome != XBTREE_RECPACKING_EMPTY)
			return -EFSCORRUPTED;
	}

	trace_xrep_rgbitmap_record_free(mp, startrtx, nextrtx - 1);

	/* Set bits as needed to round startrtx up to the nearest word. */
	bit = startrtx & XREP_RTBMP_WORDMASK;
	if (bit) {
		xfs_rtblock_t	len = nextrtx - startrtx;
		unsigned int	lastbit;

		lastbit = XFS_RTMIN(bit + len, XFS_NBWORD);
		mask = (((xfs_rtword_t)1 << (lastbit - bit)) - 1) << bit;

		error = xrep_rgbitmap_or(rgb, rtx_to_wordoff(mp, startrtx),
				mask);
		if (error || lastbit - bit == len)
			return error;
		startrtx += XFS_NBWORD - bit;
	}

	/* Set bits as needed to round nextrtx down to the nearest word. */
	bit = nextrtx & XREP_RTBMP_WORDMASK;
	if (bit) {
		mask = ((xfs_rtword_t)1 << bit) - 1;

		error = xrep_rgbitmap_or(rgb, rtx_to_wordoff(mp, nextrtx),
				mask);
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
		 * Try to keep us aligned to the rtwords buffer to reduce the
		 * number of xfile writes.
		 */
		rem = wordoff & (bufwsize - 1);
		if (rem)
			wordcnt = min_t(xrep_wordcnt_t, wordcnt,
					bufwsize - rem);

		error = xfbmp_copyin(rgb, wordoff, rgb->words, wordcnt);
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
	struct xchk_rgbitmap		*rgb = priv;
	int				error = 0;

	if (xchk_should_terminate(rgb->sc, &error))
		return error;

	if (rgb->next_rgbno < rec->rm_startblock) {
		error = xrep_rgbitmap_mark_free(rgb, rec->rm_startblock);
		if (error)
			return error;
	}

	rgb->next_rgbno = max(rgb->next_rgbno,
			      rec->rm_startblock + rec->rm_blockcount);
	return 0;
}

/*
 * Walk the rtrmapbt to find all the gaps between records, and mark the gaps
 * in the realtime bitmap that we're computing.
 */
STATIC int
xrep_rgbitmap_find_freespace(
	struct xchk_rgbitmap	*rgb)
{
	struct xfs_scrub	*sc = rgb->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_rtgroup	*rtg = sc->sr.rtg;
	int			error;

	/* Prepare a buffer of ones so that we can accelerate bulk setting. */
	memset(rgb->words, 0xFF, mp->m_sb.sb_blocksize);

	xrep_rtgroup_btcur_init(sc, &sc->sr);
	error = xfs_rmap_query_all(sc->sr.rmap_cur, xrep_rgbitmap_walk_rtrmap,
			rgb);
	if (error)
		goto out;

	/*
	 * Mark as free every possible rt extent from the last one we saw to
	 * the end of the rt group.
	 */
	if (rgb->next_rgbno < rtg->rtg_blockcount) {
		error = xrep_rgbitmap_mark_free(rgb, rtg->rtg_blockcount);
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
	struct xchk_rgbitmap	*rgb = data;
	struct xfs_mount	*mp = sc->mp;
	union xfs_rtword_raw	*ondisk;
	int			error;

	rgb->args.mp = sc->mp;
	rgb->args.tp = sc->tp;
	rgb->args.rbmbp = bp;
	ondisk = xfs_rbmblock_wordptr(&rgb->args, 0);
	rgb->args.rbmbp = NULL;

	error = xfbmp_copyout(rgb, rgb->prep_wordoff, ondisk,
			mp->m_blockwsize);
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

	rgb->prep_wordoff += mp->m_blockwsize;
	xfs_trans_buf_set_type(sc->tp, bp, XFS_BLFT_RTBITMAP_BUF);
	return 0;
}

/* Repair the realtime bitmap for this rt group. */
int
xrep_rgbitmap(
	struct xfs_scrub	*sc)
{
	struct xchk_rgbitmap	*rgb = sc->buf;
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
	error = xrep_rgbitmap_load_before(rgb);
	if (error)
		return error;
	error = xrep_rgbitmap_load_after(rgb);
	if (error)
		return error;

	/*
	 * Generate the new rtbitmap data.  We don't need the rtbmp information
	 * once this call is finished.
	 */
	error = xrep_rgbitmap_find_freespace(rgb);
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
	error = xrep_tempfile_prealloc(sc, rgb->group_rbmoff, rgb->group_rbmlen);
	if (error)
		return error;

	/* Last chance to abort before we start committing fixes. */
	if (xchk_should_terminate(sc, &error))
		return error;

	/* Copy the bitmap file that we generated. */
	error = xrep_tempfile_copyin(sc, rgb->group_rbmoff, rgb->group_rbmlen,
			xrep_rgbitmap_prep_buf, rgb);
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
	error = xrep_tempswap_trans_reserve(sc, XFS_DATA_FORK,
			rgb->group_rbmoff, rgb->group_rbmlen, &rgb->tempswap);
	if (error)
		return error;

	error = xrep_tempswap_contents(sc, &rgb->tempswap);
	if (error)
		return error;

	/* Free the old bitmap blocks if they are free. */
	return xrep_reap_ifork(sc, sc->tempip, XFS_DATA_FORK);
}

/* rt bitmap file repairs */

/* Set up to repair the realtime bitmap file metadata. */
int
xrep_setup_rtbitmap(
	struct xfs_scrub	*sc,
	struct xchk_rtbitmap	*rtb)
{
	struct xfs_mount	*mp = sc->mp;
	unsigned long long	blocks = 0;

	/*
	 * Reserve enough blocks to write out a completely new bmbt for a
	 * maximally fragmented bitmap file.  We do not hold the rtbitmap
	 * ILOCK yet, so this is entirely speculative.
	 */
	blocks = xfs_bmbt_calc_size(mp, mp->m_sb.sb_rbmblocks);
	if (blocks > UINT_MAX)
		return -EOPNOTSUPP;

	rtb->resblks += blocks;
	return 0;
}

/*
 * Make sure that the given range of the data fork of the realtime file is
 * mapped to written blocks.  The caller must ensure that the inode is joined
 * to the transaction.
 */
STATIC int
xrep_rtbitmap_data_mappings(
	struct xfs_scrub	*sc,
	xfs_filblks_t		len)
{
	struct xfs_bmbt_irec	map;
	xfs_fileoff_t		off = 0;
	int			error;

	ASSERT(sc->ip != NULL);

	while (off < len) {
		int		nmaps = 1;

		/*
		 * If we have a real extent mapping this block then we're
		 * in ok shape.
		 */
		error = xfs_bmapi_read(sc->ip, off, len - off, &map, &nmaps,
				XFS_DATA_FORK);
		if (error)
			return error;
		if (nmaps == 0) {
			ASSERT(nmaps != 0);
			return -EFSCORRUPTED;
		}

		/*
		 * Written extents are ok.  Holes are not filled because we
		 * do not know the freespace information.
		 */
		if (xfs_bmap_is_written_extent(&map) ||
		    map.br_startblock == HOLESTARTBLOCK) {
			off = map.br_startoff + map.br_blockcount;
			continue;
		}

		/*
		 * If we find a delalloc reservation then something is very
		 * very wrong.  Bail out.
		 */
		if (map.br_startblock == DELAYSTARTBLOCK)
			return -EFSCORRUPTED;

		/* Make sure we're really converting an unwritten extent. */
		if (map.br_state != XFS_EXT_UNWRITTEN) {
			ASSERT(map.br_state == XFS_EXT_UNWRITTEN);
			return -EFSCORRUPTED;
		}

		/* Make sure this block has a real zeroed extent mapped. */
		nmaps = 1;
		error = xfs_bmapi_write(sc->tp, sc->ip, map.br_startoff,
				map.br_blockcount,
				XFS_BMAPI_CONVERT | XFS_BMAPI_ZERO,
				0, &map, &nmaps);
		if (error)
			return error;
		if (nmaps != 1)
			return -EFSCORRUPTED;

		/* Commit new extent and all deferred work. */
		error = xrep_defer_finish(sc);
		if (error)
			return error;

		off = map.br_startoff + map.br_blockcount;
	}

	return 0;
}

/* Fix broken rt volume geometry. */
STATIC int
xrep_rtbitmap_geometry(
	struct xfs_scrub	*sc,
	struct xchk_rtbitmap	*rtb)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_trans	*tp = sc->tp;

	/* Superblock fields */
	if (mp->m_sb.sb_rextents != rtb->rextents)
		xfs_trans_mod_sb(sc->tp, XFS_TRANS_SB_REXTENTS,
				rtb->rextents - mp->m_sb.sb_rextents);

	if (mp->m_sb.sb_rbmblocks != rtb->rbmblocks)
		xfs_trans_mod_sb(tp, XFS_TRANS_SB_RBMBLOCKS,
				rtb->rbmblocks - mp->m_sb.sb_rbmblocks);

	if (mp->m_sb.sb_rextslog != rtb->rextslog)
		xfs_trans_mod_sb(tp, XFS_TRANS_SB_REXTSLOG,
				rtb->rextslog - mp->m_sb.sb_rextslog);

	/* Fix broken isize */
	sc->ip->i_disk_size = roundup_64(sc->ip->i_disk_size,
					 mp->m_sb.sb_blocksize);

	if (sc->ip->i_disk_size < XFS_FSB_TO_B(mp, rtb->rbmblocks))
		sc->ip->i_disk_size = XFS_FSB_TO_B(mp, rtb->rbmblocks);

	xfs_trans_log_inode(sc->tp, sc->ip, XFS_ILOG_CORE);
	return xrep_roll_trans(sc);
}

/* Repair the realtime bitmap file metadata. */
int
xrep_rtbitmap(
	struct xfs_scrub	*sc)
{
	struct xchk_rtbitmap	*rtb = sc->buf;
	struct xfs_mount	*mp = sc->mp;
	unsigned long long	blocks = 0;
	int			error;

	/* Impossibly large rtbitmap means we can't touch the filesystem. */
	if (rtb->rbmblocks > U32_MAX)
		return 0;

	/*
	 * If the size of the rt bitmap file is larger than what we reserved,
	 * figure out if we need to adjust the block reservation in the
	 * transaction.
	 */
	blocks = xfs_bmbt_calc_size(mp, rtb->rbmblocks);
	if (blocks > UINT_MAX)
		return -EOPNOTSUPP;
	if (blocks > rtb->resblks) {
		error = xfs_trans_reserve_more(sc->tp, blocks, 0);
		if (error)
			return error;

		rtb->resblks += blocks;
	}

	/* Fix inode core and forks. */
	error = xrep_metadata_inode_forks(sc);
	if (error)
		return error;

	xfs_trans_ijoin(sc->tp, sc->ip, 0);

	/* Ensure no unwritten extents. */
	error = xrep_rtbitmap_data_mappings(sc, rtb->rbmblocks);
	if (error)
		return error;

	/* Fix inconsistent bitmap geometry */
	return xrep_rtbitmap_geometry(sc, rtb);
}
