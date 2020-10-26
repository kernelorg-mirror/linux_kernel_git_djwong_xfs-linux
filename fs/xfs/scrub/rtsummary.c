// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
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
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/xfile.h"

/*
 * Realtime Summary
 * ================
 *
 * We check the realtime summary by scanning the realtime bitmap file to create
 * a new summary file incore, and then we compare the computed version against
 * the ondisk version.  We use the 'xfile' functionality to store this
 * (potentially large) amount of data in pageable memory.
 */

struct xchk_rtsum_compute {
	/* How far have we iterated through the rt extents? */
	xfs_rtblock_t		rt_extent_nr;

	/* How many free rt extents have we seen? */
	xfs_rtblock_t		rt_free_nr;

	/* block and bit offset of our current position in the rtbitmap. */
	xfs_fileoff_t		off;
	unsigned int		bit;

	/* block and bit offset of the start of the most recent free rtext. */
	xfs_fileoff_t		start_off;
	unsigned int		start_bit;

	/* Are we accumulating a free rtext? */
	bool			in_extent;
};

/* Set us up to check the rtsummary file. */
int
xchk_setup_rtsummary(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	struct xfs_mount	*mp = sc->mp;
	int			error;

	/*
	 * Create an xfile to construct a new rtsummary file.  The xfile allows
	 * us to avoid pinning kernel memory for this purpose.
	 */
	sc->xfile = xfile_create("rtsummary", mp->m_rsumsize);
	if (IS_ERR(sc->xfile))
		return PTR_ERR(sc->xfile);

	error = xchk_trans_alloc(sc, 0);
	if (error)
		return error;

	/* Allocate a memory buffer for the summary comparison. */
	sc->buf = kmem_alloc_large(sc->mp->m_sb.sb_blocksize, KM_MAYFAIL);
	if (!sc->buf)
		return -ENOMEM;

	/*
	 * Locking order requires us to take the rtbitmap first.  We must be
	 * careful to unlock it ourselves when we are done with the rtbitmap
	 * file since the scrub infrastructure won't do that for us.
	 */
	xfs_ilock(mp->m_rbmip, XFS_ILOCK_SHARED | XFS_ILOCK_RTBITMAP);

	/* ...and then we can lock the rtsummary inode. */
	sc->ilock_flags = XFS_ILOCK_EXCL | XFS_ILOCK_RTSUM;
	sc->ip = sc->mp->m_rsumip;
	xfs_ilock(sc->ip, sc->ilock_flags);

	return 0;
}

/* Update the summary file to reflect the free extent that we've accumulated. */
STATIC int
xchk_rtsum_record_free(
	struct xfs_scrub	*sc,
	struct xchk_rtsum_compute *state)
{
	struct xfs_mount	*mp = sc->mp;
	uint64_t		len;
	unsigned int		offs;
	unsigned int		log;
	unsigned int		bitsperblock = mp->m_sb.sb_blocksize * NBBY;
	xfs_suminfo_t		v = 0;
	int			error;

	/* Compute the relevant location in the rtsum file. */
	len = (state->off * bitsperblock + state->bit) -
	      (state->start_off * bitsperblock + state->start_bit);
	log = XFS_RTBLOCKLOG(len);
	offs = XFS_SUMOFFS(mp, log, state->start_off);

	/* Read current rtsummary contents. */
	error = xfile_pread(sc->xfile, &v, sizeof(xfs_suminfo_t),
			sizeof(xfs_suminfo_t) * offs);
	if (error)
		return error;

	/* Bump the summary count... */
	v++;
	trace_xchk_rtsum_record_free(mp,
			state->start_off * bitsperblock + state->start_bit,
			state->off * bitsperblock + state->bit - 1,
			len, log, offs, v);

	/* ...and write it back. */
	error = xfile_pwrite(sc->xfile, &v, sizeof(xfs_suminfo_t),
			sizeof(xfs_suminfo_t) * offs);
	if (error)
		return error;

	state->in_extent = false;
	return 0;
}

static inline bool
xchk_rtsum_isset(
	xfs_rtword_t	*words,
	unsigned int	bit)
{
	return words[bit / (sizeof(*words) * NBBY)] &
			(1ULL << (bit % (sizeof(*words) * NBBY)));
}

/* Walk a single rtbitmap block looking for changes in the free status. */
STATIC int
xchk_rtsum_process_bmblock(
	struct xfs_scrub	*sc,
	xfs_fileoff_t		block_off,
	struct xchk_rtsum_compute *state)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_buf		*bp;
	xfs_rtword_t		*words;
	unsigned int		bitsperblock = mp->m_sb.sb_blocksize * NBBY;
	int			error = 0;

	if (xchk_should_terminate(sc, &error))
		return error;

	error = xfs_rtbuf_get(mp, sc->tp, block_off, 0, &bp);
	if (!xchk_fblock_xref_process_error(sc, XFS_DATA_FORK, block_off,
				&error))
		return error;

	state->off = block_off;
	words = (xfs_rtword_t *)bp->b_addr;
	for (state->bit = 0;
	     state->bit < bitsperblock &&
	     state->rt_extent_nr < mp->m_sb.sb_rextents;
	     state->bit++, state->rt_extent_nr++) {
		if (xchk_rtsum_isset(words, state->bit)) {
			state->rt_free_nr++;
			if (!state->in_extent) {
				state->start_off = block_off;
				state->start_bit = state->bit;
				state->in_extent = true;
			}
		} else if (state->in_extent) {
			error = xchk_rtsum_record_free(sc, state);
			if (error)
				goto out_relse;
		}
	}

out_relse:
	xfs_trans_brelse(sc->tp, bp);
	return error;
}

/*
 * Compute the realtime summary from the realtime bitmap.  This is a kernel
 * port of the defunct process_rtbitmap function in xfs_repair.
 */
STATIC int
xchk_rtsum_compute(
	struct xfs_scrub	*sc)
{
	struct xchk_rtsum_compute state = { 0 };
	struct xfs_mount	*mp = sc->mp;
	unsigned long long	rtbmp_bytes;
	xfs_fileoff_t		off = 0;
	xfs_fileoff_t		end_off;
	int			error;

	rtbmp_bytes = howmany_64(mp->m_sb.sb_rextents, NBBY);
	end_off = howmany_64(rtbmp_bytes, mp->m_sb.sb_blocksize);

	/* If the bitmap size doesn't match the computed size, bail. */
	if (roundup_64(rtbmp_bytes, mp->m_sb.sb_blocksize) !=
			mp->m_rbmip->i_d.di_size)
		return -EFSCORRUPTED;

	for (off = 0; off < end_off; off++) {
		error = xchk_rtsum_process_bmblock(sc, off, &state);
		if (error)
			return error;
		if (state.rt_extent_nr == mp->m_sb.sb_rextents)
			break;
	}
	if (state.in_extent) {
		error = xchk_rtsum_record_free(sc, &state);
		if (error)
			return error;
	}

	return 0;
}

/* Compare the rtsummary file against the one we computed. */
STATIC int
xchk_rtsum_compare(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_buf		*bp;
	struct xfs_bmbt_irec	map;
	xfs_rtblock_t		off;
	loff_t			pos;
	int			nmap;
	int			error = 0;

	for (off = 0, pos = 0;
	     pos < mp->m_rsumsize;
	     pos += mp->m_sb.sb_blocksize, off++) {
		size_t		count;

		if (xchk_should_terminate(sc, &error) ||
		    (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT))
			break;

		/* Make sure we have a written extent. */
		nmap = 1;
		error = xfs_bmapi_read(mp->m_rsumip, off, 1, &map, &nmap,
				XFS_DATA_FORK);
		if (!xchk_fblock_process_error(sc, XFS_DATA_FORK, off, &error))
			break;

		if (nmap != 1 || !xfs_bmap_is_written_extent(&map)) {
			xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, off);
			break;
		}

		/* Read a block's worth of ondisk rtsummary file. */
		error = xfs_rtbuf_get(mp, sc->tp, off, 1, &bp);
		if (!xchk_fblock_process_error(sc, XFS_DATA_FORK, off, &error))
			break;

		/* Read a block's worth of computed rtsummary file. */
		count = min_t(loff_t, mp->m_rsumsize - pos,
				mp->m_sb.sb_blocksize);
		error = xfile_pread(sc->xfile, sc->buf, count, pos);
		if (error) {
			xfs_trans_brelse(sc->tp, bp);
			break;
		}

		if (memcmp(bp->b_addr, sc->buf, count) != 0)
			xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, off);

		xfs_trans_brelse(sc->tp, bp);
	}

	return error;
}

/* Scrub the realtime summary. */
int
xchk_rtsummary(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	int			error = 0;

	/* Invoke the fork scrubber. */
	error = xchk_metadata_inode_forks(sc);
	if (error || (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT))
		goto out_rbm;

	/* Construct the new summary file from the rtbitmap. */
	error = xchk_rtsum_compute(sc);
	if (error == -EFSCORRUPTED) {
		/*
		 * EFSCORRUPTED means the rtbitmap is corrupt, which is an xref
		 * error since we're checking the summary file.
		 */
		xchk_ino_xref_set_corrupt(sc, mp->m_rbmip->i_ino);
		error = 0;
		goto out_rbm;
	}
	if (error)
		goto out_rbm;

	/* Does the computed summary file match the actual rtsummary file? */
	error = xchk_rtsum_compare(sc);

out_rbm:
	/* Unlock the rtbitmap since we're done with it. */
	xfs_iunlock(mp->m_rbmip, XFS_ILOCK_SHARED | XFS_ILOCK_RTBITMAP);
	return error;
}
