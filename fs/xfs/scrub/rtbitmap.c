// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017 Oracle.  All Rights Reserved.
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
#include "xfs_rtbitmap.h"
#include "xfs_inode.h"
#include "xfs_bmap.h"
#include "xfs_rtgroup.h"
#include "xfs_rmap.h"
#include "xfs_rtrmap_btree.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/repair.h"
#include "scrub/btree.h"

/* Set us up with the realtime group metadata locked. */
int
xchk_setup_rgbitmap(
	struct xfs_scrub	*sc)
{
	int			error;

	if (xchk_need_fshook_drain(sc))
		xchk_fshooks_enable(sc, XCHK_FSHOOKS_DRAIN);

	error = xchk_trans_alloc(sc, 0);
	if (error)
		return error;

	error = xchk_install_live_inode(sc, sc->mp->m_rbmip);
	if (error)
		return error;

	return xchk_rtgroup_init(sc, sc->sm->sm_agno, &sc->sr);
}

/* Set us up with the realtime metadata locked. */
int
xchk_setup_rtbitmap(
	struct xfs_scrub	*sc)
{
	unsigned int		resblks = 0;
	int			error;

	if (xchk_could_repair(sc)) {
		error = xrep_setup_rtbitmap(sc, &resblks);
		if (error)
			return error;
	}

	error = xchk_trans_alloc(sc, resblks);
	if (error)
		return error;

	error = xchk_install_live_inode(sc, sc->mp->m_rbmip);
	if (error)
		return error;

	return xchk_rt_init(sc, &sc->sr);
}

/* Realtime bitmap. */

struct xchk_rtbitmap {
	struct xfs_scrub	*sc;

	/* The next free rt block that we expect to see. */
	xfs_rtblock_t		next_free_rtblock;
};

/* Cross-reference rtbitmap entries with other metadata. */
STATIC void
xchk_rtbitmap_xref(
	struct xchk_rtbitmap	*rtb,
	xfs_rtblock_t		startblock,
	xfs_rtblock_t		blockcount)
{
	struct xfs_scrub	*sc = rtb->sc;
	xfs_rgnumber_t		rgno;
	xfs_rgblock_t		rgbno;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;
	if (!sc->sr.rmap_cur)
		return;

	rgbno = xfs_rtb_to_rgbno(sc->mp, startblock, &rgno);
	xchk_xref_has_no_rt_owner(sc, rgbno, blockcount);

	if (rtb->next_free_rtblock < startblock) {
		xfs_rgblock_t	next_rgbno;

		next_rgbno = xfs_rtb_to_rgbno(sc->mp, rtb->next_free_rtblock,
				&rgno);
		xchk_xref_has_rt_owner(sc, next_rgbno, rgbno - next_rgbno);
	}

	rtb->next_free_rtblock = startblock + blockcount;
}

/* Scrub a free extent record from the realtime bitmap. */
STATIC int
xchk_rtbitmap_rec(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	const struct xfs_rtalloc_rec *rec,
	void			*priv)
{
	struct xchk_rtbitmap	*rtb = priv;
	struct xfs_scrub	*sc = rtb->sc;
	xfs_rtblock_t		startblock;
	xfs_filblks_t		blockcount;

	startblock = xfs_rtx_to_rtb(mp, rec->ar_startext);
	blockcount = xfs_rtx_to_rtb(mp, rec->ar_extcount);

	if (!xfs_verify_rtbext(mp, startblock, blockcount))
		xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, 0);

	xchk_rtbitmap_xref(rtb, startblock, blockcount);

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return -ECANCELED;

	return 0;
}

/* Make sure the entire rtbitmap file is mapped with written extents. */
STATIC int
xchk_rtbitmap_check_extents(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_bmbt_irec	map;
	xfs_fileoff_t		off;
	int			nmap;
	int			error = 0;

	for (off = 0; off < mp->m_sb.sb_rbmblocks;) {
		if (xchk_should_terminate(sc, &error) ||
		    (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT))
			break;

		/* Make sure we have a written extent. */
		nmap = 1;
		error = xfs_bmapi_read(mp->m_rbmip, off,
				mp->m_sb.sb_rbmblocks - off, &map, &nmap,
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

/* Scrub this group's realtime bitmap. */
int
xchk_rgbitmap(
	struct xfs_scrub	*sc)
{
	struct xfs_rtalloc_rec	keys[2];
	struct xchk_rtbitmap	rtb = {
		.sc		= sc,
	};
	struct xfs_rtgroup	*rtg = sc->sr.rtg;
	xfs_rtblock_t		rtbno;
	xfs_rtblock_t		last_rtbno;
	xfs_rgblock_t		last_rgbno = rtg->rtg_blockcount - 1;
	int			error;

	/* Sanity check the realtime bitmap size. */
	if (sc->mp->m_rbmip->i_disk_size !=
	    XFS_FSB_TO_B(sc->mp, sc->mp->m_sb.sb_rbmblocks)) {
		xchk_ino_set_corrupt(sc, sc->mp->m_rbmip->i_ino);
		return 0;
	}

	/*
	 * Check only the portion of the rtbitmap that corresponds to this
	 * realtime group.
	 */
	rtbno = xfs_rgbno_to_rtb(sc->mp, rtg->rtg_rgno, 0);
	rtb.next_free_rtblock = rtbno;
	keys[0].ar_startext = xfs_rtb_to_rtxt(sc->mp, rtbno);

	rtbno = xfs_rgbno_to_rtb(sc->mp, rtg->rtg_rgno, last_rgbno);
	keys[1].ar_startext = xfs_rtb_to_rtxt(sc->mp, rtbno);
	keys[0].ar_extcount = keys[1].ar_extcount = 0;

	error = xfs_rtalloc_query_range(sc->mp, sc->tp, &keys[0], &keys[1],
			xchk_rtbitmap_rec, &rtb);
	if (!xchk_fblock_process_error(sc, XFS_DATA_FORK, 0, &error))
		return error;

	/*
	 * Check that the are rmappings for all rt extents between the end of
	 * the last free extent we saw and the last possible extent in the rt
	 * group.
	 */
	last_rtbno = xfs_rgbno_to_rtb(sc->mp, rtg->rtg_rgno, last_rgbno);
	if (rtb.next_free_rtblock < last_rtbno) {
		xfs_rgnumber_t	rgno;
		xfs_rgblock_t	next_rgbno;

		next_rgbno = xfs_rtb_to_rgbno(sc->mp, rtb.next_free_rtblock,
				&rgno);
		xchk_xref_has_rt_owner(sc, next_rgbno,
				last_rgbno - next_rgbno);
	}

	return 0;
}

/* Scrub the realtime bitmap. */
int
xchk_rtbitmap(
	struct xfs_scrub	*sc)
{
	struct xchk_rtbitmap	rtb = {
		.sc		= sc,
	};
	int			error;

	/* Is the size of the rtbitmap correct? */
	if (sc->mp->m_rbmip->i_disk_size !=
	    XFS_FSB_TO_B(sc->mp, sc->mp->m_sb.sb_rbmblocks)) {
		xchk_ino_set_corrupt(sc, sc->mp->m_rbmip->i_ino);
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
	if (xfs_has_rtgroups(sc->mp))
		return 0;

	error = xfs_rtalloc_query_all(sc->mp, sc->tp, xchk_rtbitmap_rec, &rtb);
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

	startext = xfs_rtb_to_rtxt(sc->mp, rtbno);
	endext = xfs_rtb_to_rtxt(sc->mp, rtbno + len - 1);
	error = xfs_rtalloc_extent_is_free(sc->mp, sc->tp, startext,
			endext - startext + 1, &is_free);
	if (!xchk_should_check_xref(sc, &error, NULL))
		return;
	if (is_free)
		xchk_ino_xref_set_corrupt(sc, sc->mp->m_rbmip->i_ino);
}
