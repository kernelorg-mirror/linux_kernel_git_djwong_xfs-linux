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
#include "xfs_rtalloc.h"
#include "xfs_inode.h"
#include "xfs_bmap.h"
#include "xfs_rmap.h"
#include "xfs_rtrmap_btree.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"

/* Set us up with the realtime metadata locked. */
int
xchk_setup_rtbitmap(
	struct xfs_scrub	*sc)
{
	int			error;

	error = xchk_trans_alloc(sc, 0);
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

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;

	xchk_xref_has_no_rt_owner(sc, startblock, blockcount);

	if (rtb->next_free_rtblock < startblock)
		xchk_xref_has_rt_owner(sc, rtb->next_free_rtblock,
				startblock - rtb->next_free_rtblock);

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
	xfs_rtblock_t		blockcount;

	startblock = rec->ar_startext * mp->m_sb.sb_rextsize;
	blockcount = rec->ar_extcount * mp->m_sb.sb_rextsize;

	if (!xfs_verify_rtext(mp, startblock, blockcount))
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
	xfs_rtblock_t		off;
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

/* Scrub the realtime bitmap. */
int
xchk_rtbitmap(
	struct xfs_scrub	*sc)
{
	struct xchk_rtbitmap	rtb = {
		.sc		= sc,
		.next_free_rtblock = 0,
	};
	struct xfs_mount	*mp = sc->mp;
	xfs_rtblock_t		last_rblock;
	int			error;

	/* Is the size of the rtbitmap correct? */
	if (mp->m_rbmip->i_disk_size !=
	    XFS_FSB_TO_B(mp, mp->m_sb.sb_rbmblocks)) {
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

	error = xfs_rtalloc_query_all(sc->mp, sc->tp, xchk_rtbitmap_rec, &rtb);
	if (!xchk_fblock_process_error(sc, XFS_DATA_FORK, 0, &error))
		goto out;

	/*
	 * Check that the are rmappings for all rt extents between the end of
	 * the last free extent we saw and the last possible extent in the rt
	 * volume.
	 */
	last_rblock = rounddown_64(mp->m_sb.sb_rblocks, mp->m_sb.sb_rextsize);
	if (rtb.next_free_rtblock < last_rblock)
		xchk_xref_has_rt_owner(sc, rtb.next_free_rtblock,
				last_rblock - rtb.next_free_rtblock);
out:
	return error;
}

/* xref check that the extent is not free in the rtbitmap */
void
xchk_xref_is_used_rt_space(
	struct xfs_scrub	*sc,
	xfs_rtblock_t		fsbno,
	xfs_extlen_t		len)
{
	xfs_rtblock_t		startext;
	xfs_rtblock_t		endext;
	xfs_rtblock_t		extcount;
	bool			is_free;
	int			error;

	if (xchk_skip_xref(sc->sm))
		return;

	startext = fsbno;
	endext = fsbno + len - 1;
	do_div(startext, sc->mp->m_sb.sb_rextsize);
	do_div(endext, sc->mp->m_sb.sb_rextsize);
	extcount = endext - startext + 1;
	error = xfs_rtalloc_extent_is_free(sc->mp, sc->tp, startext, extcount,
			&is_free);
	if (!xchk_should_check_xref(sc, &error, NULL))
		return;
	if (is_free)
		xchk_ino_xref_set_corrupt(sc, sc->mp->m_rbmip->i_ino);
}
