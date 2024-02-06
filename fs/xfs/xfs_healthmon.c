// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_trace.h"
#include "xfs_health.h"
#include "xfs_ag.h"
#include "xfs_btree.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_quota_defs.h"
#include "xfs_rtgroup.h"
#include "xfs_healthmon.h"

/*
 * Live Health Monitoring
 * ======================
 *
 * Autonomous self-healing of XFS filesystems requires a means for the kernel
 * to send filesystem health events to a monitoring daemon in userspace.  To
 * accomplish this, we establish a thread_with_file kthread object to handle
 * translating internal events about filesystem health into a format that can
 * be parsed easily by userspace.  Then we hook various parts of the filesystem
 * to supply those internal events to the kthread.  Userspace reads events
 * from the file descriptor returned by the ioctl.
 *
 * The healthmon abstraction has a weak reference to the host filesystem mount
 * so that the queueing and processing of the events do not pin the mount and
 * cannot slow down the main filesystem.  The healthmon object can exist past
 * the end of the filesystem mount.
 */

struct xfs_healthmon {
	/* thread with stdio redirection */
	struct thread_with_stdio	thread;
};

static inline struct xfs_healthmon *
to_healthmon(struct thread_with_stdio	*thr)
{
	return container_of(thr, struct xfs_healthmon, thread);
}

/* Free the health monitoring information. */
STATIC void
xfs_healthmon_exit(
	struct thread_with_stdio	*thr)
{
	struct xfs_healthmon		*hm = to_healthmon(thr);

	kfree(hm);
	module_put(THIS_MODULE);
}

/* Pipe health monitoring information to userspace. */
STATIC void
xfs_healthmon_run(
	struct thread_with_stdio	*thr)
{
}

/* Validate ioctl parameters. */
static inline bool
xfs_healthmon_validate(
	const struct xfs_health_monitor	*hmo)
{
	if (hmo->flags)
		return false;
	if (hmo->format)
		return false;
	if (memchr_inv(&hmo->pad1, 0, sizeof(hmo->pad1)))
		return false;
	if (memchr_inv(&hmo->pad2, 0, sizeof(hmo->pad2)))
		return false;
	return true;
}

static const struct thread_with_stdio_ops xfs_healthmon_ops = {
	.exit		= xfs_healthmon_exit,
	.fn		= xfs_healthmon_run,
};

/*
 * Create a health monitoring file.  Returns an index to the fd table or a
 * negative errno.
 */
int
xfs_healthmon_create(
	struct xfs_mount		*mp,
	struct xfs_health_monitor	*hmo)
{
	struct xfs_healthmon		*hm;
	int				ret;

	if (!xfs_healthmon_validate(hmo))
		return -EINVAL;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!try_module_get(THIS_MODULE))
		return -ENOMEM;

	hm = kzalloc(sizeof(*hm), GFP_KERNEL);
	if (!hm) {
		ret = -ENOMEM;
		goto out_mod;
	}

	ret = run_thread_with_stdout(&hm->thread, &xfs_healthmon_ops);
	if (ret < 0)
		goto out_hm;

	return ret;
out_hm:
	kfree(hm);
out_mod:
	module_put(THIS_MODULE);
	return ret;
}
