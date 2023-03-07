// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2017-2023 Oracle.  All Rights Reserved.
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
#include "xfs_rtbitmap.h"
#include "xfs_inode.h"
#include "xfs_bmap.h"
#include "xfs_bit.h"
#include "xfs_rtgroup.h"
#include "xfs_rmap.h"
#include "xfs_rtrmap_btree.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/repair.h"
#include "scrub/rtbitmap.h"
#include "scrub/btree.h"

static inline void
xchk_rtbitmap_compute_geometry(
	struct xfs_mount	*mp,
	struct xchk_rtbitmap	*rtb)
{
	rtb->rextents = xfs_rtb_to_rtx(mp, mp->m_sb.sb_rblocks);
	rtb->rextslog = rtb->rextents ? xfs_highbit32(rtb->rextents) : 0;
	rtb->rbmblocks = xfs_rtbitmap_blockcount(mp, rtb->rextents);
}

/* Set us up with the realtime group metadata locked. */
int
xchk_setup_rgbitmap(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xchk_rgbitmap	*rgb;
	int			error;

	rgb = kzalloc(sizeof(struct xchk_rgbitmap), XCHK_GFP_FLAGS);
	if (!rgb)
		return -ENOMEM;
	rgb->sc = sc;
	sc->buf = rgb;

	error = xchk_trans_alloc(sc, 0);
	if (error)
		return error;

	error = xchk_install_live_inode(sc, mp->m_rbmip);
	if (error)
		return error;

	error = xchk_ino_dqattach(sc);
	if (error)
		return error;

	error = xchk_rtgroup_init(sc, sc->sm->sm_agno, &sc->sr,
			XCHK_RTGLOCK_ALL);
	if (error)
		return error;

	/*
	 * Now that we've locked the rtbitmap, we can't race with growfsrt
	 * trying to expand the bitmap or change the size of the rt volume.
	 * Hence it is safe to compute and check the geometry values.
	 */
	xchk_rtbitmap_compute_geometry(mp, &rgb->rtb);
	return 0;
}

/* Set us up with the realtime metadata locked. */
int
xchk_setup_rtbitmap(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xchk_rtbitmap	*rtb;
	int			error;

	if (xchk_need_intent_drain(sc))
		xchk_fsgates_enable(sc, XCHK_FSGATES_DRAIN);

	rtb = kzalloc(sizeof(struct xchk_rtbitmap), XCHK_GFP_FLAGS);
	if (!rtb)
		return -ENOMEM;
	sc->buf = rtb;

	if (xchk_could_repair(sc)) {
		error = xrep_setup_rtbitmap(sc, rtb);
		if (error)
			return error;
	}

	error = xchk_trans_alloc(sc, rtb->resblks);
	if (error)
		return error;

	error = xchk_install_live_inode(sc, sc->mp->m_rbmip);
	if (error)
		return error;

	error = xchk_ino_dqattach(sc);
	if (error)
		return error;

	xchk_rt_init(sc, &sc->sr, XCHK_RTLOCK_BITMAP);

	/*
	 * Now that we've locked the rtbitmap, we can't race with growfsrt
	 * trying to expand the bitmap or change the size of the rt volume.
	 * Hence it is safe to compute and check the geometry values.
	 */
	xchk_rtbitmap_compute_geometry(mp, rtb);
	return 0;
}

/* Per-rtgroup bitmap contents. */

/* Cross-reference rtbitmap entries with other metadata. */
STATIC void
xchk_rgbitmap_xref(
	struct xchk_rgbitmap	*rgb,
	xfs_rtblock_t		startblock,
	xfs_rtblock_t		blockcount)
{
	struct xfs_scrub	*sc = rgb->sc;
	xfs_rgnumber_t		rgno;
	xfs_rgblock_t		rgbno;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;
	if (!sc->sr.rmap_cur)
		return;

	rgbno = xfs_rtb_to_rgbno(sc->mp, startblock, &rgno);
	xchk_xref_has_no_rt_owner(sc, rgbno, blockcount);

	if (rgb->next_free_rtblock < startblock) {
		xfs_rgblock_t	next_rgbno;

		next_rgbno = xfs_rtb_to_rgbno(sc->mp, rgb->next_free_rtblock,
				&rgno);
		xchk_xref_has_rt_owner(sc, next_rgbno, rgbno - next_rgbno);
	}

	rgb->next_free_rtblock = startblock + blockcount;
}

/* Scrub a free extent record from the realtime bitmap. */
STATIC int
xchk_rgbitmap_rec(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	const struct xfs_rtalloc_rec *rec,
	void			*priv)
{
	struct xchk_rgbitmap	*rgb = priv;
	struct xfs_scrub	*sc = rgb->sc;
	xfs_rtblock_t		startblock;
	xfs_filblks_t		blockcount;

	startblock = xfs_rtx_to_rtb(mp, rec->ar_startext);
	blockcount = xfs_rtx_to_rtb(mp, rec->ar_extcount);

	if (!xfs_verify_rtbext(mp, startblock, blockcount))
		xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, 0);

	xchk_rgbitmap_xref(rgb, startblock, blockcount);

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return -ECANCELED;

	return 0;
}

/* Scrub this group's realtime bitmap. */
int
xchk_rgbitmap(
	struct xfs_scrub	*sc)
{
	struct xfs_rtalloc_rec	keys[2];
	struct xfs_mount	*mp = sc->mp;
	struct xfs_rtgroup	*rtg = sc->sr.rtg;
	struct xchk_rgbitmap	*rgb = sc->buf;
	xfs_rtblock_t		rtbno;
	xfs_rtblock_t		last_rtbno;
	xfs_rgblock_t		last_rgbno = rtg->rtg_blockcount - 1;
	int			error;

	/* Sanity check the realtime bitmap size. */
	if (sc->ip->i_disk_size < XFS_FSB_TO_B(mp, rgb->rtb.rbmblocks)) {
		xchk_ino_set_corrupt(sc, sc->ip->i_ino);
		return 0;
	}

	/*
	 * Check only the portion of the rtbitmap that corresponds to this
	 * realtime group.
	 */
	rtbno = xfs_rgbno_to_rtb(mp, rtg->rtg_rgno, 0);
	rgb->next_free_rtblock = rtbno;
	keys[0].ar_startext = xfs_rtb_to_rtx(mp, rtbno);

	rtbno = xfs_rgbno_to_rtb(mp, rtg->rtg_rgno, last_rgbno);
	keys[1].ar_startext = xfs_rtb_to_rtx(mp, rtbno);
	keys[0].ar_extcount = keys[1].ar_extcount = 0;

	error = xfs_rtalloc_query_range(mp, sc->tp, &keys[0], &keys[1],
			xchk_rgbitmap_rec, rgb);
	if (!xchk_fblock_process_error(sc, XFS_DATA_FORK, 0, &error))
		return error;

	/*
	 * Check that the are rmappings for all rt extents between the end of
	 * the last free extent we saw and the last possible extent in the rt
	 * group.
	 */
	last_rtbno = xfs_rgbno_to_rtb(sc->mp, rtg->rtg_rgno, last_rgbno);
	if (rgb->next_free_rtblock < last_rtbno) {
		xfs_rgnumber_t	rgno;
		xfs_rgblock_t	next_rgbno;

		next_rgbno = xfs_rtb_to_rgbno(sc->mp, rgb->next_free_rtblock,
				&rgno);
		xchk_xref_has_rt_owner(sc, next_rgbno,
				last_rgbno - next_rgbno);
	}

	return 0;
}

/* Realtime bitmap. */

/* Scrub a free extent record from the realtime bitmap. */
STATIC int
xchk_rtbitmap_rec(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	const struct xfs_rtalloc_rec *rec,
	void			*priv)
{
	struct xfs_scrub	*sc = priv;
	xfs_rtblock_t		startblock;
	xfs_filblks_t		blockcount;

	startblock = xfs_rtx_to_rtb(mp, rec->ar_startext);
	blockcount = xfs_rtx_to_rtb(mp, rec->ar_extcount);

	if (!xfs_verify_rtbext(mp, startblock, blockcount))
		xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, 0);
	return 0;
}

/* Make sure the entire rtbitmap file is mapped with written extents. */
STATIC int
xchk_rtbitmap_check_extents(
	struct xfs_scrub	*sc)
{
	struct xfs_bmbt_irec	map;
	struct xfs_iext_cursor	icur;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_inode	*ip = sc->ip;
	xfs_fileoff_t		off = 0;
	xfs_fileoff_t		endoff;
	int			error = 0;

	/* Mappings may not cross or lie beyond EOF. */
	endoff = XFS_B_TO_FSB(mp, ip->i_disk_size);
	if (xfs_iext_lookup_extent(ip, &ip->i_df, endoff, &icur, &map)) {
		xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, endoff);
		return 0;
	}

	while (off < endoff) {
		int		nmap = 1;

		if (xchk_should_terminate(sc, &error) ||
		    (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT))
			break;

		/* Make sure we have a written extent. */
		error = xfs_bmapi_read(ip, off, endoff - off, &map, &nmap,
				XFS_DATA_FORK);
		if (!xchk_fblock_process_error(sc, XFS_DATA_FORK, off, &error))
			break;

		if (nmap != 1 || !xfs_bmap_is_written_extent(&map)) {
			xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, off);
			break;
		}

		off += map.br_blockcount;
	}

	return error;
}

/* Scrub the realtime bitmap. */
int
xchk_rtbitmap(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xchk_rtbitmap	*rtb = sc->buf;
	int			error;

	/* Is sb_rextents correct? */
	if (mp->m_sb.sb_rextents != rtb->rextents) {
		xchk_ino_set_corrupt(sc, mp->m_rbmip->i_ino);
		return 0;
	}

	/* Is sb_rextslog correct? */
	if (mp->m_sb.sb_rextslog != rtb->rextslog) {
		xchk_ino_set_corrupt(sc, mp->m_rbmip->i_ino);
		return 0;
	}

	/*
	 * Is sb_rbmblocks large enough to handle the current rt volume?  In no
	 * case can we exceed 4bn bitmap blocks since the super field is a u32.
	 */
	if (rtb->rbmblocks > U32_MAX) {
		xchk_ino_set_corrupt(sc, mp->m_rbmip->i_ino);
		return 0;
	}
	if (mp->m_sb.sb_rbmblocks != rtb->rbmblocks) {
		xchk_ino_set_corrupt(sc, mp->m_rbmip->i_ino);
		return 0;
	}

	/* The bitmap file length must be aligned to an fsblock. */
	if (mp->m_rbmip->i_disk_size & mp->m_blockmask) {
		xchk_ino_set_corrupt(sc, mp->m_rbmip->i_ino);
		return 0;
	}

	/*
	 * Is the bitmap file itself large enough to handle the rt volume?
	 * growfsrt expands the bitmap file before updating sb_rextents, so the
	 * file can be larger than sb_rbmblocks.
	 */
	if (mp->m_rbmip->i_disk_size < XFS_FSB_TO_B(mp, rtb->rbmblocks)) {
		xchk_ino_set_corrupt(sc, mp->m_rbmip->i_ino);
		return 0;
	}

	/* Invoke the fork scrubber. */
	error = xchk_metadata_inode_forks(sc);
	if (error || (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT))
		return error;

	error = xchk_rtbitmap_check_extents(sc);
	if (error || (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT))
		return error;

	/*
	 * Each rtgroup checks its portion of the rt bitmap, so if we don't
	 * have that feature, we have to check the bitmap contents now.
	 */
	if (xfs_has_rtgroups(mp))
		return 0;

	error = xfs_rtalloc_query_all(mp, sc->tp, xchk_rtbitmap_rec, sc);
	if (!xchk_fblock_process_error(sc, XFS_DATA_FORK, 0, &error))
		return error;

	return 0;
}

/* xref check that the extent is not free in the rtbitmap */
void
xchk_xref_is_used_rt_space(
	struct xfs_scrub	*sc,
	xfs_rtblock_t		rtbno,
	xfs_extlen_t		len)
{
	xfs_rtxnum_t		startext;
	xfs_rtxnum_t		endext;
	bool			is_free;
	int			error;

	if (xchk_skip_xref(sc->sm))
		return;

	startext = xfs_rtb_to_rtx(sc->mp, rtbno);
	endext = xfs_rtb_to_rtx(sc->mp, rtbno + len - 1);
	error = xfs_rtalloc_extent_is_free(sc->mp, sc->tp, startext,
			endext - startext + 1, &is_free);
	if (!xchk_should_check_xref(sc, &error, NULL))
		return;
	if (is_free)
		xchk_ino_xref_set_corrupt(sc, sc->mp->m_rbmip->i_ino);
}
