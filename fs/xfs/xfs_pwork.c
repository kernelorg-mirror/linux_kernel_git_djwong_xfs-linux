// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_trace.h"
#include "xfs_sysctl.h"
#include "xfs_pwork.h"
#include <linux/nmi.h>

/*
 * Parallel Work Queue
 * ===================
 *
 * Abstract away the details of running a large and "obviously" parallelizable
 * task across multiple CPUs.  Callers initialize the pwork control object with
 * a desired level of parallelization and a work function.  Next, they embed
 * struct xfs_pwork in whatever structure they use to pass work context to a
 * worker thread and queue that pwork.  The work function will be passed the
 * pwork item when it is run (from process context) and any returned error will
 * be recorded in xfs_pwork_ctl.error.  Work functions should check for errors
 * and abort if necessary; the non-zeroness of xfs_pwork_ctl.error does not
 * stop workqueue item processing.
 *
 * This is the rough equivalent of the xfsprogs workqueue code, though we can't
 * reuse that name here.
 */

/* Invoke our caller's function. */
static void
xfs_pwork_work(
	struct work_struct	*work)
{
	struct xfs_pwork	*pwork;
	struct xfs_pwork_ctl	*pctl;
	int			error;

	pwork = container_of(work, struct xfs_pwork, work);
	pctl = pwork->pctl;
	error = pctl->work_fn(pctl->mp, pwork);
	if (error && !pctl->error)
		pctl->error = error;
	if (atomic_dec_and_test(&pctl->nr_work))
		wake_up(&pctl->poll_wait);
}

/*
 * Set up control data for parallel work.  @work_fn is the function that will
 * be called.  @tag will be written into the kernel threads.  @nr_threads is
 * the level of parallelism desired, or 0 for no limit.
 */
int
xfs_pwork_init(
	struct xfs_mount	*mp,
	struct xfs_pwork_ctl	*pctl,
	xfs_pwork_work_fn	work_fn,
	const char		*tag,
	unsigned int		nr_threads)
{
#ifdef DEBUG
	if (xfs_globals.pwork_threads >= 0)
		nr_threads = xfs_globals.pwork_threads;
#endif
	trace_xfs_pwork_init(mp, nr_threads, current->pid);

	pctl->wq = alloc_workqueue("%s-%d", WQ_FREEZABLE, nr_threads, tag,
			current->pid);
	if (!pctl->wq)
		return -ENOMEM;
	pctl->work_fn = work_fn;
	pctl->error = 0;
	pctl->mp = mp;
	atomic_set(&pctl->nr_work, 0);
	init_waitqueue_head(&pctl->poll_wait);

	return 0;
}

/* Queue some parallel work. */
void
xfs_pwork_queue(
	struct xfs_pwork_ctl	*pctl,
	struct xfs_pwork	*pwork)
{
	INIT_WORK(&pwork->work, xfs_pwork_work);
	pwork->pctl = pctl;
	atomic_inc(&pctl->nr_work);
	queue_work(pctl->wq, &pwork->work);
}

/* Wait for the work to finish and tear down the control structure. */
int
xfs_pwork_destroy(
	struct xfs_pwork_ctl	*pctl)
{
	destroy_workqueue(pctl->wq);
	pctl->wq = NULL;
	return pctl->error;
}

/*
 * Wait for the work to finish by polling completion status and touch the soft
 * lockup watchdog.  This is for callers such as mount which hold locks.
 */
void
xfs_pwork_poll(
	struct xfs_pwork_ctl	*pctl)
{
	while (wait_event_timeout(pctl->poll_wait,
				atomic_read(&pctl->nr_work) == 0, HZ) == 0)
		touch_softlockup_watchdog();
}

/* Estimate the amount of parallelism available for a storage device. */
static unsigned int
xfs_guess_buftarg_parallelism(
	struct xfs_buftarg	*btp)
{
	int			iomin;
	int			ioopt;

	/*
	 * The device tells us that it is non-rotational, and we take that to
	 * mean there are no moving parts and that the device can handle all
	 * the CPUs throwing IO requests at it.
	 */
	if (blk_queue_nonrot(btp->bt_bdev->bd_disk->queue))
		return num_online_cpus();

	/*
	 * The device has a preferred and minimum IO size that suggest a RAID
	 * setup, so infer the number of disks and assume that the parallelism
	 * is equal to the disk count.
	 */
	iomin = bdev_io_min(btp->bt_bdev);
	ioopt = bdev_io_opt(btp->bt_bdev);
	if (iomin > 0 && ioopt > iomin)
		return ioopt / iomin;

	/*
	 * The device did not indicate that it has any capabilities beyond that
	 * of a rotating disk with a single drive head, so we estimate no
	 * parallelism at all.
	 */
	return 1;
}

/*
 * Estimate the amount of parallelism that is available for metadata operations
 * on this filesystem.
 */
unsigned int
xfs_pwork_guess_metadata_threads(
	struct xfs_mount	*mp)
{
	unsigned int		threads;

	/*
	 * Estimate the amount of parallelism for metadata operations from the
	 * least capable of the two devices that handle metadata.  Cap that
	 * estimate to the number of AGs to avoid unnecessary lock contention.
	 */
	threads = xfs_guess_buftarg_parallelism(mp->m_ddev_targp);
	if (mp->m_logdev_targp != mp->m_ddev_targp)
		threads = min(xfs_guess_buftarg_parallelism(mp->m_logdev_targp),
			      threads);
	threads = min(mp->m_sb.sb_agcount, threads);

	/* If the storage told us it has fancy capabilities, we're done. */
	if (threads > 1)
		goto clamp;

	/*
	 * Metadata storage did not even hint that it has any parallel
	 * capability.  If the filesystem was formatted with a stripe unit and
	 * width, we'll treat that as evidence of a RAID setup and estimate
	 * the number of disks.
	 */
	if (mp->m_sb.sb_unit > 0 && mp->m_sb.sb_width > mp->m_sb.sb_unit)
		threads = mp->m_sb.sb_width / mp->m_sb.sb_unit;

clamp:
	/* Don't return an estimate larger than the CPU count. */
	return min(num_online_cpus(), threads);
}

/* Estimate how many threads we need for a parallel work queue. */
unsigned int
xfs_pwork_guess_workqueue_threads(
	struct xfs_mount	*mp)
{
	/* pwork queues are not unbounded, so we have to abide WQ_MAX_ACTIVE. */
	return min_t(unsigned int, xfs_pwork_guess_metadata_threads(mp),
			WQ_MAX_ACTIVE);
}
