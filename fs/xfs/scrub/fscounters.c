// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
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
#include "xfs_health.h"
#include "xfs_bmap.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"

/*
 * FS Summary Counters
 * ===================
 *
 * The basics of filesystem summary counter checking are that we iterate the
 * AGs counting the number of free blocks, free space btree blocks, per-AG
 * reservations, inodes, delayed allocation reservations, and free inodes.
 * Then we compare what we computed against the in-core counters.
 *
 * However, the reality is that summary counters are a tricky beast to check.
 * While we /could/ freeze the filesystem and scramble around the AGs counting
 * the free blocks, in practice we prefer not do that for a scan because
 * freezing is costly.  To get around this, we added a per-cpu counter of the
 * delalloc reservations so that we can rotor around the AGs relatively
 * quickly, and we allow the counts to be slightly off because we're not
 * taking any locks while we do this.
 */

int
xchk_setup_fscounters(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	int			error;

	sc->buf = kmem_zalloc(sizeof(struct xchk_fscounters), KM_SLEEP);
	if (!sc->buf)
		return -ENOMEM;

	/*
	 * Pause background reclaim while we're scrubbing to reduce the
	 * likelihood of background perturbations to the counters throwing
	 * off our calculations.
	 *
	 * If we're repairing, we need to prevent any other thread from
	 * changing the global fs summary counters while we're repairing them.
	 * This requires the fs to be frozen, which will disable background
	 * reclaim and purge all inactive inodes.
	 */
	if (sc->sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR) {
		error = xchk_fs_freeze(sc);
		if (error)
			return error;
	} else {
		xchk_disable_reclaim(sc);
	}

	return xchk_trans_alloc(sc, 0);
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
	uint64_t		delayed;
	xfs_agnumber_t		agno;
	int			error;

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		/* Lock both AG headers. */
		error = xfs_ialloc_read_agi(mp, sc->tp, agno, &agi_bp);
		if (error)
			return error;
		error = xfs_alloc_read_agf(mp, sc->tp, agno, 0, &agf_bp);
		if (error)
			return error;
		if (!agf_bp)
			return -ENOMEM;

		/* Count all the inodes */
		agi = XFS_BUF_TO_AGI(agi_bp);
		fsc->icount += be32_to_cpu(agi->agi_count);
		fsc->ifree += be32_to_cpu(agi->agi_freecount);

		/* Add up the free/freelist/bnobt/cntbt blocks */
		agf = XFS_BUF_TO_AGF(agf_bp);
		fsc->fdblocks += be32_to_cpu(agf->agf_freeblks);
		fsc->fdblocks += be32_to_cpu(agf->agf_flcount);
		fsc->fdblocks += be32_to_cpu(agf->agf_btreeblks);

		/*
		 * Per-AG reservations are taken out of the incore counters,
		 * so they must be left out of the free blocks computation.
		 */
		pag = xfs_perag_get(mp, agno);
		fsc->fdblocks -= pag->pag_meta_resv.ar_reserved;
		fsc->fdblocks -= pag->pag_rmapbt_resv.ar_orig_reserved;
		xfs_perag_put(pag);

		xfs_trans_brelse(sc->tp, agf_bp);
		xfs_trans_brelse(sc->tp, agi_bp);
	}

	/*
	 * The global incore space reservation is taken from the incore
	 * counters, so leave that out of the computation.
	 */
	fsc->fdblocks -= mp->m_resblks_avail;

	/*
	 * Delayed allocation reservations are taken out of the incore counters
	 * but not recorded on disk, so leave them and their indlen blocks out
	 * of the computation.
	 */
	delayed = percpu_counter_sum(&mp->m_delayed_blks);
	fsc->fdblocks -= delayed;

	trace_xchk_fscounters_calc(mp, fsc->icount, fsc->ifree, fsc->fdblocks,
			delayed);

	/* Bail out if the values we compute are totally nonsense. */
	if (!xfs_verify_icount(mp, fsc->icount) ||
	    fsc->fdblocks > mp->m_sb.sb_dblocks ||
	    fsc->ifree > fsc->icount)
		return -EFSCORRUPTED;

	return 0;
}

/*
 * Is the @counter within an acceptable range of @expected?
 *
 * Currently that means 1/16th (6%) or @nr_range of the @expected value.
 * If we're repairing then we require an exact match.
 */
static inline bool
xchk_fscounter_within_range(
	struct xfs_scrub	*sc,
	struct percpu_counter	*counter,
	uint64_t		expected,
	uint64_t		nr_range)
{
	int64_t			value = percpu_counter_sum(counter);
	uint64_t		range;

	if (sc->sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR)
		range = 0;
	else
		range = max_t(uint64_t, expected >> 4, nr_range);
	if (value < 0)
		return false;
	if (range < expected && value < expected - range)
		return false;
	if ((int64_t)(expected + range) >= 0 && value > expected + range)
		return false;
	return true;
}

/* Check the superblock counters. */
int
xchk_fscounters(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xchk_fscounters	*fsc = sc->buf;
	int64_t			icount, ifree, fdblocks;
	int			error;

	icount = percpu_counter_sum(&sc->mp->m_icount);
	ifree = percpu_counter_sum(&sc->mp->m_ifree);
	fdblocks = percpu_counter_sum(&sc->mp->m_fdblocks);

	if (icount < 0 || ifree < 0 || fdblocks < 0)
		xchk_block_set_corrupt(sc, mp->m_sb_bp);

	/* See if icount is obviously wrong. */
	if (!xfs_verify_icount(mp, icount))
		xchk_block_set_corrupt(sc, mp->m_sb_bp);

	/* See if fdblocks / ifree are obviously wrong. */
	if (fdblocks > mp->m_sb.sb_dblocks)
		xchk_block_set_corrupt(sc, mp->m_sb_bp);
	if (ifree > icount)
		xchk_block_set_corrupt(sc, mp->m_sb_bp);

	/* If we already know it's bad, we can skip the AG iteration. */
	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return 0;

	/* Counters seem ok, but let's count them. */
	error = xchk_fscounters_calc(sc, fsc);
	if (!xchk_process_error(sc, 0, XFS_SB_BLOCK(sc->mp), &error))
		return error;

	/*
	 * Compare the in-core counters with whatever we counted.  We'll
	 * consider the inode counts ok if they're within 1024 inodes, and the
	 * free block counts if they're within 1/64th of the filesystem size.
	 */
	if (!xchk_fscounter_within_range(sc, &mp->m_icount, fsc->icount, 1024))
		xchk_block_set_corrupt(sc, mp->m_sb_bp);

	if (!xchk_fscounter_within_range(sc, &mp->m_ifree, fsc->ifree, 1024))
		xchk_block_set_corrupt(sc, mp->m_sb_bp);

	if (!xchk_fscounter_within_range(sc, &mp->m_fdblocks, fsc->fdblocks,
			mp->m_sb.sb_dblocks >> 6))
		xchk_block_set_corrupt(sc, mp->m_sb_bp);

	return 0;
}
