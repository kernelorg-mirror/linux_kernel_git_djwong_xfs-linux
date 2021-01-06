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
#include "scrub/xfile.h"
#include "scrub/repair.h"

/* Set us up with the realtime metadata locked. */
int
xchk_setup_rtbitmap(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	unsigned long long	resblks = 0;
	int			error;

	error = xrep_setup_tempfile(sc, S_IFREG);
	if (error)
		return error;

	/*
	 * If we're doing a repair, we reserve 2x the bitmap blocks: once for
	 * the new bitmap contents and again for the bmbt blocks and the
	 * remapping operation.
	 */
	if (sc->sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR) {
		loff_t		bmp_bytes;

		/* Create an xfile to hold our reconstructed bitmap. */
		bmp_bytes = XFS_FSB_TO_B(sc->mp, sc->mp->m_sb.sb_rbmblocks);
		sc->xfile = xfile_create("rtbitmap", bmp_bytes);
		if (IS_ERR(sc->xfile))
			return PTR_ERR(sc->xfile);

		resblks = sc->mp->m_sb.sb_rbmblocks * 2;
		if (resblks > UINT_MAX)
			return -EOPNOTSUPP;

		/*
		 * Allocate a memory buffer for faster creation of the new
		 * bitmap.
		 */
		sc->buf = kmem_alloc_large(sc->mp->m_sb.sb_blocksize,
				KM_MAYFAIL);
		if (!sc->buf)
			return -ENOMEM;
	}

	error = xchk_trans_alloc(sc, resblks);
	if (error)
		return error;

	error = xchk_scan_this_inode(sc, sc->mp->m_rbmip);
	if (error)
		return error;

	xchk_rt_init(sc, &sc->sr);
	return 0;
}

/* Realtime bitmap. */

/* Cross-reference rtbitmap entries with other metadata. */
STATIC void
xchk_rtbitmap_xref(
	struct xfs_scrub	*sc,
	xfs_rtblock_t		startblock,
	xfs_rtblock_t		blockcount)
{
	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;

	xchk_xref_has_no_rt_owner(sc, startblock, blockcount);
	xchk_xref_is_not_shared_rt(sc, startblock, blockcount);
}

/* Scrub a free extent record from the realtime bitmap. */
STATIC int
xchk_rtbitmap_rec(
	struct xfs_trans	*tp,
	struct xfs_rtalloc_rec	*rec,
	void			*priv)
{
	struct xfs_scrub	*sc = priv;
	xfs_rtblock_t		startblock;
	xfs_rtblock_t		blockcount;

	startblock = rec->ar_startext * tp->t_mountp->m_sb.sb_rextsize;
	blockcount = rec->ar_extcount * tp->t_mountp->m_sb.sb_rextsize;

	if (!xfs_verify_rtext(sc->mp, startblock, blockcount))
		xchk_fblock_set_corrupt(sc, XFS_DATA_FORK, 0);

	xchk_rtbitmap_xref(sc, startblock, blockcount);
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
	int			error;

	/* Is the size of the rtbitmap correct? */
	if (sc->mp->m_rbmip->i_d.di_size !=
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

	error = xfs_rtalloc_query_all(sc->tp, xchk_rtbitmap_rec, sc);
	if (!xchk_fblock_process_error(sc, XFS_DATA_FORK, 0, &error))
		goto out;

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
