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
 *
 * So the first thing we do is warm up the buffer cache in the setup routine
 * by walking all the AGs to make sure the incore per-AG structure has been
 * initialized.  The expected value calculation then iterates the incore per-AG
 * structures as quickly as it can.  We snapshot the percpu counters before and
 * after this operation and use the difference in counter values to guess at
 * our tolerance for mismatch between expected and actual counter values.
 */

/*
 * Since the expected value computation is lockless but only browses incore
 * values, the percpu counters should be fairly close to each other.  However,
 * we'll allow ourselves to be off by at least this (arbitrary) amount.
 */
#define XCHK_FSC_MIN_VARIANCE	(512)

/*
 * Make sure the per-AG structure has been initialized from the on-disk header
 * contents and that the incore counters match the ondisk counters.  Do this
 * from the setup function so that the inner summation loop runs as quickly
 * as possible.
 */
STATIC int
xchk_fsc_warmup(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_buf		*agi_bp = NULL;
	struct xfs_buf		*agf_bp = NULL;
	struct xfs_agi		*agi;
	struct xfs_agf		*agf;
	struct xfs_perag	*pag = NULL;
	xfs_agnumber_t		agno;
	int			error = 0;

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		/* Lock both AG headers. */
		error = xfs_ialloc_read_agi(mp, sc->tp, agno, &agi_bp);
		if (error)
			break;
		error = xfs_alloc_read_agf(mp, sc->tp, agno, 0, &agf_bp);
		if (error)
			break;
		error = -ENOMEM;
		if (!agf_bp)
			break;

		pag = xfs_perag_get(mp, agno);

		/*
		 * These are supposed to be initialized by the header read
		 * function.
		 */
		error = -EFSCORRUPTED;
		if (!pag->pagi_init || !pag->pagf_init)
			break;

		/* Spot-check the incore counters against the ondisk headers */
		agi = XFS_BUF_TO_AGI(agi_bp);
		agf = XFS_BUF_TO_AGF(agf_bp);
		if (pag->pagi_count != be32_to_cpu(agi->agi_count))
			break;
		if (pag->pagi_freecount != be32_to_cpu(agi->agi_freecount))
			break;
		if (pag->pagf_freeblks != be32_to_cpu(agf->agf_freeblks))
			break;
		if (pag->pagf_flcount != be32_to_cpu(agf->agf_flcount))
			break;
		if (pag->pagf_btreeblks != be32_to_cpu(agf->agf_btreeblks))
			break;

		xfs_perag_put(pag);
		pag = NULL;
		xfs_buf_relse(agf_bp);
		agf_bp = NULL;
		xfs_buf_relse(agi_bp);
		agi_bp = NULL;
		error = 0;
	}

	if (pag)
		xfs_perag_put(pag);
	if (agf_bp)
		xfs_buf_relse(agf_bp);
	if (agi_bp)
		xfs_buf_relse(agi_bp);
	return error;
}

int
xchk_setup_fscounters(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	int			error;

	sc->buf = kmem_zalloc(sizeof(struct xchk_fscounters), KM_SLEEP);
	if (!sc->buf)
		return -ENOMEM;

	/* We must get the incore counters set up before we can proceed. */
	error = xchk_fsc_warmup(sc);
	if (error)
		return error;

	/*
	 * Pause background reclaim while we're scrubbing to reduce the
	 * likelihood of background perturbations to the counters throwing
	 * off our calculations.
	 */
	xchk_stop_reaping(sc);

	return xchk_trans_alloc(sc, 0);
}

/*
 * Calculate what the global in-core counters ought to be from the incore
 * per-AG structure.  Callers can compare this to the actual in-core counters
 * to estimate by how much both in-core and on-disk counters need to be
 * adjusted.
 */
STATIC int
xchk_fsc_calc(
	struct xfs_scrub	*sc,
	struct xchk_fscounters	*fsc)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_perag	*pag;
	unsigned long long	min_icount, max_icount;
	uint64_t		delayed;
	xfs_agnumber_t		agno;
	int			tries = 8;

retry:
	fsc->icount = 0;
	fsc->ifree = 0;
	fsc->fdblocks = 0;

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		pag = xfs_perag_get(mp, agno);

		/* This somehow got unset since the warmup? */
		if (!pag->pagi_init || !pag->pagf_init) {
			xfs_perag_put(pag);
			return -EFSCORRUPTED;
		}

		/* Count all the inodes */
		fsc->icount += pag->pagi_count;
		fsc->ifree += pag->pagi_freecount;

		/* Add up the free/freelist/bnobt/cntbt blocks */
		fsc->fdblocks += pag->pagf_freeblks;
		fsc->fdblocks += pag->pagf_flcount;
		fsc->fdblocks += pag->pagf_btreeblks;

		/*
		 * Per-AG reservations are taken out of the incore counters,
		 * so they must be left out of the free blocks computation.
		 */
		fsc->fdblocks -= pag->pag_meta_resv.ar_reserved;
		fsc->fdblocks -= pag->pag_rmapbt_resv.ar_orig_reserved;

		xfs_perag_put(pag);
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
	delayed = percpu_counter_sum(&mp->m_delalloc_blks);
	fsc->fdblocks -= delayed;

	trace_xchk_fscounters_calc(mp, fsc->icount, fsc->ifree, fsc->fdblocks,
			delayed);

	xfs_icount_range(mp, &min_icount, &max_icount);

	/* Bail out if the values we compute are totally nonsense. */
	if (!xfs_verify_icount(mp, fsc->icount) ||
	    fsc->fdblocks > mp->m_sb.sb_dblocks ||
	    fsc->ifree > max_icount)
		return -EFSCORRUPTED;

	/*
	 * If ifree > icount then we probably had some perturbation in the
	 * counters while we were calculating things.  We'll try a few times
	 * to maintain ifree <= icount before giving up.
	 */
	if (fsc->ifree > fsc->icount) {
		if (tries--)
			goto retry;
		return -EFSCORRUPTED;
	}

	return 0;
}

/*
 * Is the @counter reasonably close to the @expected value?
 *
 * We neither locked nor froze anything in the filesystem while calculating
 * @expected, which means that the counter could have changed.  We know the
 * @old_value of the counter, which means that we also know how much the counter
 * changed while calculating the expected value, and we'll take that as a rough
 * guess at how accurate we have to be.  The expected value calculation runs in
 * memory and never does any IO, so the window ought to be pretty small.
 *
 * First, ensure that @counter must never be negative.
 *
 * Next, compute the variance from the expected value that we'll accept.
 * For now we'll use twice the change in the counter from start to finish
 * or the minimum variance, whichever is larger.
 */
static inline bool
xchk_fsc_within_range(
	struct xfs_scrub	*sc,
	const int64_t		old_value,
	struct percpu_counter	*counter,
	uint64_t		expected)
{
	int64_t			curr_value = percpu_counter_sum(counter);
	int64_t			range;

	range = abs(2 * (curr_value - old_value));
	if (range < XCHK_FSC_MIN_VARIANCE)
		range = XCHK_FSC_MIN_VARIANCE;

	trace_xchk_fscounters_within_range(sc->mp, expected, curr_value,
			old_value, range);

	if (curr_value < 0)
		return false;
	if (curr_value == expected)
		return true;
	if (range < expected && curr_value < expected - range)
		return false;
	if ((int64_t)(expected + range) >= 0 && curr_value > expected + range)
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

	/* Snapshot the percpu counters. */
	icount = percpu_counter_sum(&mp->m_icount);
	ifree = percpu_counter_sum(&mp->m_ifree);
	fdblocks = percpu_counter_sum(&mp->m_fdblocks);

	/* No negative values, please! */
	if (icount < 0 || ifree < 0 || fdblocks < 0)
		xchk_set_corrupt(sc);

	/* See if icount is obviously wrong. */
	if (!xfs_verify_icount(mp, icount))
		xchk_set_corrupt(sc);

	/* See if fdblocks is obviously wrong. */
	if (fdblocks > mp->m_sb.sb_dblocks)
		xchk_set_corrupt(sc);

	/*
	 * If ifree exceeds icount by more than the minimum variance then
	 * something's probably wrong with the counters.
	 */
	if (ifree > icount && ifree - icount > XCHK_FSC_MIN_VARIANCE)
		xchk_set_corrupt(sc);

	/* Walk the incore AG headers to calculate the expected counters. */
	error = xchk_fsc_calc(sc, fsc);
	if (!xchk_process_error(sc, 0, XFS_SB_BLOCK(mp), &error))
		return error;

	/* Compare the in-core counters with whatever we counted. */
	if (!xchk_fsc_within_range(sc, icount, &mp->m_icount, fsc->icount))
		xchk_set_corrupt(sc);

	if (!xchk_fsc_within_range(sc, ifree, &mp->m_ifree, fsc->ifree))
		xchk_set_corrupt(sc);

	if (!xchk_fsc_within_range(sc, fdblocks, &mp->m_fdblocks,
				fsc->fdblocks))
		xchk_set_corrupt(sc);

	return 0;
}
