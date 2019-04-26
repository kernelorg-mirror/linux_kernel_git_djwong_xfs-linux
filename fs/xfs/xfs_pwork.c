// SPDX-License-Identifier: GPL-2.0+
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
#include "xfs_pwork.h"
#include <linux/nmi.h>

/*
 * Parallel Work Queue
 * ===================
 * Abstract away the details of running a large and "obviously" parallelizable
 * task across multiple CPUs.  Callers initialize the pwork control object with
 * a desired level of parallelization and a work function.  Next, they embed
 * struct xfs_pwork in whatever structure they use to pass work context to a
 * worker thread and queue that pwork.  The work function will be passed the
 * pwork item when it is run (from process context) and any returned error will
 * cause all threads to abort.
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
	error = pctl->work_fn(pwork);
	if (error && !pctl->error)
		pctl->error = error;
	atomic_dec(&pctl->works);
}

/*
 * Set up control data for parallel work.  @work_fn is the function that will
 * be called.  @tag will be written into the kernel threads.  @nr_threads is
 * the level of parallelism desired, or 0 for no limit.
 */
int
xfs_pwork_init(
	struct xfs_pwork_ctl	*pwctl,
	xfs_pwork_work_fn	work_fn,
	const char		*tag,
	unsigned int		nr_threads)
{
	pwctl->wq = alloc_workqueue("%s-%d", WQ_FREEZABLE, nr_threads, tag,
			current->pid);
	if (!pwctl->wq)
		return -ENOMEM;
	pwctl->work_fn = work_fn;
	pwctl->error = 0;
	atomic_set(&pwctl->works, 0);
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
	atomic_inc(&pctl->works);
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
 * Wait for the work to finish and tear down the control structure.
 * Continually poll completion status and touch the soft lockup watchdog.
 * This is for things like mount that hold locks.
 */
int
xfs_pwork_destroy_poll(
	struct xfs_pwork_ctl	*pctl)
{
	while (atomic_read(&pctl->works) > 0) {
		msleep(1);
		touch_softlockup_watchdog();
	}

	return pctl->error;
}

/*
 * Return the amount of parallelism that the data device can handle, or 0 for
 * no limit.
 */
unsigned int
xfs_pwork_guess_datadev_parallelism(
	struct xfs_mount	*mp)
{
	struct xfs_buftarg	*btp = mp->m_ddev_targp;
	int			iomin;
	int			ioopt;

	if (blk_queue_nonrot(btp->bt_bdev->bd_queue))
		return num_online_cpus();
	if (mp->m_sb.sb_width && mp->m_sb.sb_unit)
		return mp->m_sb.sb_width / mp->m_sb.sb_unit;
	iomin = bdev_io_min(btp->bt_bdev);
	ioopt = bdev_io_opt(btp->bt_bdev);
	if (iomin && ioopt)
		return ioopt / iomin;

	return 1;
}
