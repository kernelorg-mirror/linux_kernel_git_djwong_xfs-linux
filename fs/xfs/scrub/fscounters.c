// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_btree.h"
#include "xfs_bit.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_inode.h"
#include "xfs_alloc.h"
#include "xfs_ialloc.h"
#include "xfs_rmap.h"
#include "xfs_error.h"
#include "xfs_errortag.h"
#include "xfs_icache.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"

/*
 * FS Summary Counters
 * ===================
 *
 * Filesystem summary counters are a tricky beast to check.  We cannot have
 * anyone changing the superblock fields, the percpu counters, or the AG
 * headers while we do the global check.  This means that we must freeze the
 * filesystem for the entire duration.   Once that's done, we compute what the
 * incore counters /should/ be based on the counters in the AG headers
 * (presumably we checked those in an earlier part of scrub) and the in-core
 * free space reservations (both the user-changeable one and the per-AG ones).
 *
 * From there we compare the computed incore counts to the actual ones and
 * complain if they're off.  For repair we compute the deltas needed to
 * correct the counters and then update the incore and ondisk counters
 * accordingly.
 */

/* Summary counter checks require a frozen fs. */
int
xchk_setup_fscounters(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	int			error;

	/* Save counters across runs. */
	sc->buf = kmem_zalloc(sizeof(struct xchk_fscounters), KM_SLEEP);
	if (!sc->buf)
		return -ENOMEM;

	/*
	 * We need to prevent any other thread from changing the global fs
	 * summary counters while we're scrubbing or repairing them.  This
	 * requires the fs to be frozen.
	 *
	 * Scrub can do some basic sanity checks if userspace does not permit
	 * us to freeze the filesystem.
	 */
	if ((sc->sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR) &&
	    !(sc->sm->sm_flags & XFS_SCRUB_IFLAG_FREEZE_OK))
		return -EUSERS;

	/*
	 * Make sure we've purged every inactive inode in the system because
	 * our live inode walker won't touch anything that's in reclaim.
	 */
	xfs_inactive_force(sc->mp);

	if (sc->sm->sm_flags & XFS_SCRUB_IFLAG_FREEZE_OK) {
		error = xfs_scrub_fs_freeze(sc);
		if (error)
			return error;
	}

	/* Set up the scrub context. */
	return xchk_trans_alloc(sc, 0);
}

/*
 * Record the number of blocks reserved for this inode for future writes but
 * not yet allocated to real space.  In other words, we're looking for all
 * subtractions from fdblocks that aren't backed by actual space allocations
 * while we recalculate fdlbocks.
 */
STATIC int
xchk_fscounters_count_del(
	struct xfs_inode	*ip,
	void			*priv)
{
	struct xfs_iext_cursor	icur;
	struct xfs_bmbt_irec	rec;
	struct xfs_ifork	*ifp;
	uint64_t		*d = priv;
	int64_t			delblks = ip->i_delayed_blks;

	if (delblks == 0)
		return 0;

	/* Add the indlen blocks for each data fork reservation. */
	ifp = XFS_IFORK_PTR(ip, XFS_DATA_FORK);
	for_each_xfs_iext(ifp, &icur, &rec) {
		if (!isnullstartblock(rec.br_startblock))
			continue;
		delblks += startblockval(rec.br_startblock);
	}

	/*
	 * Add the indlen blocks for each CoW fork reservation.  Remember
	 * that we count real/unwritten extents in the CoW fork towards
	 * i_delayed_blks, so we have to subtract those.  If it's a delalloc
	 * reservation, add the indlen blocks instead.
	 */
	ifp = XFS_IFORK_PTR(ip, XFS_COW_FORK);
	if (ifp) {
		for_each_xfs_iext(ifp, &icur, &rec) {
			if (isnullstartblock(rec.br_startblock))
				delblks += startblockval(rec.br_startblock);
			else
				delblks -= rec.br_blockcount;
		}
	}

	/* No, we can't have negative reservations. */
	if (delblks < 0)
		return -EFSCORRUPTED;

	*d += delblks;
	return 0;
}

/*
 * Calculate what the global in-core counters ought to be from the AG header
 * contents.  Callers can compare this to the actual in-core counters to
 * calculate by how much both in-core and on-disk counters need to be
 * adjusted.
 */
STATIC int
xchk_fscounters_calc(
	struct xfs_scrub	*sc,
	struct xchk_fscounters	*fsc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_buf		*agi_bp;
	struct xfs_buf		*agf_bp;
	struct xfs_agi		*agi;
	struct xfs_agf		*agf;
	struct xfs_perag	*pag;
	uint64_t		delayed = 0;
	xfs_agnumber_t		agno;
	int			error;

	ASSERT(sc->fs_frozen);

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		/* Count all the inodes */
		error = xfs_ialloc_read_agi(mp, sc->tp, agno, &agi_bp);
		if (error)
			return error;
		agi = XFS_BUF_TO_AGI(agi_bp);
		fsc->icount += be32_to_cpu(agi->agi_count);
		fsc->ifree += be32_to_cpu(agi->agi_freecount);

		/* Add up the free/freelist/bnobt/cntbt blocks */
		error = xfs_alloc_read_agf(mp, sc->tp, agno, 0, &agf_bp);
		if (error)
			return error;
		if (!agf_bp)
			return -ENOMEM;
		agf = XFS_BUF_TO_AGF(agf_bp);
		fsc->fdblocks += be32_to_cpu(agf->agf_freeblks);
		fsc->fdblocks += be32_to_cpu(agf->agf_flcount);
		fsc->fdblocks += be32_to_cpu(agf->agf_btreeblks);

		/*
		 * Per-AG reservations are taken out of the incore counters,
		 * so count them out.
		 */
		pag = xfs_perag_get(mp, agno);
		fsc->fdblocks -= pag->pag_meta_resv.ar_reserved;
		fsc->fdblocks -= pag->pag_rmapbt_resv.ar_orig_reserved;
		xfs_perag_put(pag);
	}

	/*
	 * The global space reservation is taken out of the incore counters,
	 * so count that out too.
	 */
	fsc->fdblocks -= mp->m_resblks_avail;

	/*
	 * Delayed allocation reservations are taken out of the incore counters
	 * but not recorded on disk, so count them out too.
	 */
	error = xfs_scrub_foreach_live_inode(sc, xchk_fscounters_count_del,
			&delayed);
	if (error)
		return error;
	fsc->fdblocks -= delayed;

	trace_xchk_fscounters_calc(mp, fsc->icount, fsc->ifree,
			fsc->fdblocks, delayed);

	/* Bail out if the values we compute are totally nonsense. */
	if (!xfs_verify_icount(mp, fsc->icount) ||
	    fsc->fdblocks > mp->m_sb.sb_dblocks ||
	    fsc->ifree > fsc->icount)
		return -EFSCORRUPTED;

	return 0;
}

/*
 * Check the superblock counters.
 *
 * The filesystem must be frozen so that the counters do not change while
 * we're computing the summary counters.
 */
int
xchk_fscounters(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xchk_fscounters	*fsc = sc->buf;
	int			error;

	/* See if icount is obviously wrong. */
	if (!xfs_verify_icount(mp, mp->m_sb.sb_icount))
		xchk_block_set_corrupt(sc, mp->m_sb_bp);

	/* See if fdblocks / ifree are obviously wrong. */
	if (mp->m_sb.sb_fdblocks > mp->m_sb.sb_dblocks)
		xchk_block_set_corrupt(sc, mp->m_sb_bp);
	if (mp->m_sb.sb_ifree > mp->m_sb.sb_icount)
		xchk_block_set_corrupt(sc, mp->m_sb_bp);

	/* Did we already flag bad summary counters? */
	if (XFS_TEST_ERROR((mp->m_flags & XFS_MOUNT_BAD_SUMMARY), mp,
			XFS_ERRTAG_FORCE_SUMMARY_RECALC))
		xchk_block_set_corrupt(sc, mp->m_sb_bp);
	else if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		xfs_force_summary_recalc(sc->mp);

	/*
	 * If we're only checking for corruption and we found it, exit now.
	 *
	 * Repair depends on the counter values we collect here, so if the
	 * IFLAG_REPAIR flag is set we must continue to calculate the correct
	 * counter values.
	 */
	if (!(sc->sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR) &&
	    (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT))
		return 0;

	/* Bail out if we need to be frozen to do the hard checks. */
	if (!sc->fs_frozen) {
		xchk_set_incomplete(sc);
		return -EUSERS;
	}

	/* Counters seem ok, but let's count them. */
	error = xchk_fscounters_calc(sc, fsc);
	if (!xchk_process_error(sc, 0, XFS_SB_BLOCK(sc->mp), &error))
		return error;

	/*
	 * Compare the in-core counters.  In theory we sync'd the superblock
	 * when we did the repair freeze, so they should be the same as the
	 * percpu counters.
	 */
	spin_lock(&mp->m_sb_lock);
	if (mp->m_sb.sb_icount != fsc->icount)
		xchk_block_set_corrupt(sc, mp->m_sb_bp);
	if (mp->m_sb.sb_ifree != fsc->ifree)
		xchk_block_set_corrupt(sc, mp->m_sb_bp);
	if (mp->m_sb.sb_fdblocks != fsc->fdblocks)
		xchk_block_set_corrupt(sc, mp->m_sb_bp);
	spin_unlock(&mp->m_sb_lock);

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		xfs_force_summary_recalc(sc->mp);

	return 0;
}
