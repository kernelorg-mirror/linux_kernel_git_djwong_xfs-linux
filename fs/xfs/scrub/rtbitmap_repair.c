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
#include "xfs_refcount.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/xfile.h"
#include "scrub/tempfile.h"

struct xrep_rtbmp {
	struct xfs_scrub	*sc;

	/* The next rt block we expect to see during our rtrmapbt walk. */
	xfs_rtblock_t		next_rtbno;
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
xrep_rtbitmap_or(
	struct xrep_rtbmp	*rb,
	xfs_rtblock_t		rt_ext,
	xfs_rtword_t		mask)
{
	loff_t			pos = rtword_off(rt_ext);
	xfs_rtword_t		word;
	int			error;

	error = xfile_obj_load(rb->sc->xfile, &word, sizeof(word), pos);
	if (error)
		return error;

	trace_xrep_rtbitmap_or(rb->sc->mp, rt_ext, pos, mask, word);

	word |= mask;
	return xfile_obj_store(rb->sc->xfile, &word, sizeof(word), pos);
}

/* Mark a range free in the incore rtbitmap. */
STATIC int
xrep_rtbitmap_mark_free(
	struct xrep_rtbmp	*rb,
	xfs_rtblock_t		next)
{
	struct xfs_mount	*mp = rb->sc->mp;
	xfs_rtblock_t		start_ext;
	xfs_rtblock_t		next_ext;
	loff_t			pos, endpos;
	unsigned int		bit;
	unsigned int		mod;
	xfs_rtword_t		mask;
	bool			shared;
	int			error;

	if (!xfs_verify_rtext(mp, rb->next_rtbno, next - rb->next_rtbno))
		return -EFSCORRUPTED;

	/* Convert rt blocks to rt extents. */
	start_ext = div_u64_rem(rb->next_rtbno, mp->m_sb.sb_rextsize, &mod);
	if (mod)
		return -EFSCORRUPTED;
	next_ext = div_u64_rem(next, mp->m_sb.sb_rextsize, &mod);
	if (mod)
		return -EFSCORRUPTED;

	/* Must not be shared or CoW staging. */
	if (rb->sc->sr.refc_cur) {
		error = xfs_refcount_has_record(rb->sc->sr.refc_cur,
				rb->next_rtbno, next - rb->next_rtbno,
				&shared);
		if (error)
			return error;
		if (shared)
			return -EFSCORRUPTED;
	}

	trace_xrep_rtbitmap_record_free(mp, start_ext, next_ext - 1);

	/* Set bits as needed to round start_ext up to the nearest word. */
	bit = start_ext & (XFS_NBWORD - 1);
	if (bit) {
		xfs_rtblock_t	len = next_ext - start_ext;
		unsigned int	lastbit;

		lastbit = XFS_RTMIN(bit + len, XFS_NBWORD);
		mask = (((xfs_rtword_t)1 << (lastbit - bit)) - 1) << bit;

		error = xrep_rtbitmap_or(rb, start_ext, mask);
		if (error || lastbit - bit == len)
			return error;
		start_ext += XFS_NBWORD - bit;
	}

	/* Set bits as needed to round next_ext down to the nearest word. */
	bit = next_ext & (XFS_NBWORD - 1);
	if (bit) {
		mask = ((xfs_rtword_t)1 << bit) - 1;

		error = xrep_rtbitmap_or(rb, next_ext, mask);
		if (error || start_ext + bit == next_ext)
			return error;
		next_ext -= bit;
	}

	trace_xrep_rtbitmap_record_free_bulk(mp, start_ext, next_ext - 1);

	/* Set all the bytes in between, up to a whole fs block at once. */
	pos = start_ext >> XFS_NBBYLOG;
	endpos = next_ext >> XFS_NBBYLOG;

	while (pos < endpos) {
		loff_t	rem;
		size_t	count = min_t(loff_t, endpos - pos,
				      mp->m_sb.sb_blocksize);

		/* Try to get us aligned to an even blocksize. */
		rem = pos & (mp->m_sb.sb_blocksize - 1);
		if (rem)
			count = min_t(loff_t, count,
				      mp->m_sb.sb_blocksize - rem);

		error = xfile_obj_store(rb->sc->xfile, rb->sc->buf, count, pos);
		if (error)
			return error;
		pos += count;
	}

	return 0;
}

/* Set up to repair the realtime bitmap. */
int
xrep_setup_rtbitmap(
	struct xfs_scrub	*sc,
	unsigned int		*resblks)
{
	struct xfs_mount	*mp = sc->mp;
	unsigned long long	blocks = 0;
	loff_t			bmp_bytes;
	int			error;

	error = xrep_tempfile_create(sc, S_IFREG);
	if (error)
		return error;

	/* Create an xfile to hold our reconstructed bitmap. */
	bmp_bytes = XFS_FSB_TO_B(mp, mp->m_sb.sb_rbmblocks);
	error = xfile_create(sc->mp, "rtbitmap", bmp_bytes, &sc->xfile);
	if (error)
		return error;

	/*
	 * Allocate a memory buffer for faster creation of new bitmap
	 * blocks.
	 */
	sc->buf = kvmalloc(mp->m_sb.sb_blocksize,
			GFP_KERNEL | __GFP_NOWARN | __GFP_RETRY_MAYFAIL);
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
	return 0;
}

/* Set free space in the rtbitmap based on rtrmapbt records. */
STATIC int
xrep_rtbitmap_walk_rtrmap(
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec,
	void				*priv)
{
	struct xrep_rtbmp		*rb = priv;
	int				error = 0;

	if (xchk_should_terminate(rb->sc, &error))
		return error;

	if (rb->next_rtbno < rec->rm_startblock) {
		error = xrep_rtbitmap_mark_free(rb, rec->rm_startblock);
		if (error)
			return error;
	}

	rb->next_rtbno = max(rb->next_rtbno,
			     rec->rm_startblock + rec->rm_blockcount);
	return 0;
}

/*
 * Walk the rtrmapbt to find all the gaps between records, and mark the gaps
 * in the realtime bitmap that we're computing.
 */
STATIC int
xrep_rtbitmap_find_freespace(
	struct xrep_rtbmp	*rb)
{
	struct xfs_scrub	*sc = rb->sc;
	int			error;

	/* Prepare a buffer of ones so that we can accelerate bulk setting. */
	memset(sc->buf, 0xFF, sc->mp->m_sb.sb_blocksize);

	xrep_rt_btcur_init(sc, &sc->sr);
	error = xfs_rmap_query_all(sc->sr.rmap_cur, xrep_rtbitmap_walk_rtrmap,
			rb);
	xchk_rt_btcur_free(&sc->sr);
	if (error || sc->mp->m_sb.sb_rblocks == rb->next_rtbno)
		return error;

	/* Mark all space to the end of the rt device free. */
	return xrep_rtbitmap_mark_free(rb, sc->mp->m_sb.sb_rblocks);
}

/* Repair the realtime bitmap. */
int
xrep_rtbitmap(
	struct xfs_scrub	*sc)
{
	struct xrep_rtbmp	rb = {
		.sc		= sc,
	};
	struct xfs_swapext_req	req;
	int			error;

	/*
	 * We require the realtime rmapbt (and atomic file updates) to rebuild
	 * anything.
	 */
	if (!xfs_has_rtrmapbt(sc->mp))
		return -EOPNOTSUPP;

	/* Generate the new rtbitmap data. */
	error = xrep_rtbitmap_find_freespace(&rb);
	if (error)
		return error;

	/* Make sure any problems with the fork are fixed. */
	error = xrep_metadata_inode_forks(sc);
	if (error)
		return error;

	/*
	 * Trylock the temporary file.  We had better be the only ones holding
	 * onto this inode...
	 */
	if (!xrep_tempfile_ilock_nowait(sc, XFS_ILOCK_EXCL))
		return -EAGAIN;

	/* Make sure we have space allocated for the entire bitmap file. */
	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	xfs_trans_ijoin(sc->tp, sc->tempip, 0);
	error = xrep_tempfile_prealloc(sc, 0, sc->mp->m_sb.sb_rbmblocks);
	if (error)
		return error;

	/* Last chance to abort before we start committing fixes. */
	if (xchk_should_terminate(sc, &error))
		return error;

	/* Copy the bitmap file that we generated. */
	error = xrep_tempfile_copyin_xfile(sc, &xfs_rtbuf_ops,
			XFS_BLFT_RTBITMAP_BUF,
			XFS_FSB_TO_B(sc->mp, sc->mp->m_sb.sb_rbmblocks));
	if (error)
		return error;

	/* Now swap the extents. */
	error = xrep_tempfile_swapext_prep_request(sc, XFS_DATA_FORK, &req);
	if (error)
		return error;

	error = xrep_tempfile_swapext(sc, &req);
	if (error)
		return error;

	/* Stale old buffers and truncate the file. */
	return xrep_reap_fork(sc, sc->tempip, XFS_DATA_FORK);
}
