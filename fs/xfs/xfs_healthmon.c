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
#include "xfs_ag.h"
#include "xfs_btree.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_quota_defs.h"
#include "xfs_rtgroup.h"
#include "xfs_fsops.h"
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
 *
 * The easily parseable event format is a stream of json objects as follows:
 *
 * Queue Management
 * ----------------
 *
 * {
 *	"type": "lost" or "shutdown",
 *	"domain": "mount",
 *	"time_ns": integer
 * }
 *
 * "lost" indicates that the kthread dropped events due to memory allocation
 * failures or queue limits.
 *
 * "mount" means that the event affects the entire filesystem mount.
 *
 * "time_ns" is the time stamp of when the event originated in the kernel,
 * expressed in nanoseconds.
 *
 * Abnormal Shutdowns
 * ------------------
 *
 * {
 *	"type": "shutdown",
 *	"domain": "mount",
 *	"reasons": [reason_string list...],
 *	"time_ns": integer
 * }
 *
 * "shutdown" indicates that the filesystem shut down either due to errors or
 * due to an explicit request from userspace.
 *
 * "reasons" are a list of strings describing why the filesystem went down.
 * They correspond to the SHUTDOWN_* flags.
 */

#define XFS_HEALTHMON_MAX_EVENTS \
		(32768 / sizeof(struct xfs_healthmon_event))

struct flag_string {
	unsigned int	mask;
	const char	*str;
};

struct xfs_healthmon {
	/* thread with stdio redirection */
	struct thread_with_stdio	thread;

	/* lock for mp and eventlist */
	struct mutex			lock;

	/* waiter for signalling the arrival of events */
	struct wait_queue_head		wait;

	/* list of event objects */
	struct xfs_healthmon_event	*first_event;
	struct xfs_healthmon_event	*last_event;

	/* live update hooks */
	struct xfs_shutdown_hook	shook;

	/* filesystem mount, or NULL if we've unmounted */
	struct xfs_mount		*mp;

	/* number of events */
	unsigned int			events;

	/* do we want all events? */
	bool				verbose;

	/* did we lose an event? */
	bool				lost_prev_event;
};

static inline struct xfs_healthmon *
to_healthmon(struct thread_with_stdio	*thr)
{
	return container_of(thr, struct xfs_healthmon, thread);
}

/* Free all events */
STATIC void
xfs_healthmon_free_events(
	struct xfs_healthmon		*hm)
{
	struct xfs_healthmon_event	*event, *next;

	event = hm->first_event;
	while (event != NULL) {
		trace_xfs_healthmon_drop(hm->mp, event);
		next = event->next;
		kfree(event);
		event = next;
	}
	hm->first_event = hm->last_event = NULL;
}

/* Free the health monitoring information. */
STATIC void
xfs_healthmon_exit(
	struct thread_with_stdio	*thr)
{
	struct xfs_healthmon		*hm = to_healthmon(thr);

	trace_xfs_healthmon_exit(hm->mp, hm->events, hm->lost_prev_event);

	if (hm->mp) {
		xfs_shutdown_hook_del(hm->mp, &hm->shook);
	}
	xfs_shutdown_hook_disable();
	mutex_destroy(&hm->lock);
	xfs_healthmon_free_events(hm);
	kfree(hm);
	module_put(THIS_MODULE);
}

/* Remove an event from the head of the list. */
static inline struct xfs_healthmon_event *
xfs_healthmon_pop(
	struct xfs_healthmon		*hm)
{
	struct xfs_healthmon_event	*ret = hm->first_event;

	if (!ret)
		return NULL;

	if (hm->last_event == ret)
		hm->last_event = NULL;
	hm->first_event = ret->next;
	hm->events--;

	trace_xfs_healthmon_pop(hm->mp, ret);
	return ret;
}

/* Push an event onto the end of the list. */
static inline void
xfs_healthmon_push(
	struct xfs_healthmon		*hm,
	struct xfs_healthmon_event	*event)
{
	if (!hm->first_event)
		hm->first_event = event;
	if (hm->last_event)
		hm->last_event->next = event;
	hm->last_event = event;
	event->next = NULL;
	hm->events++;
	wake_up(&hm->wait);

	trace_xfs_healthmon_push(hm->mp, event);
}

/* Create a new event or record that we failed. */
static struct xfs_healthmon_event *
new_event(
	struct xfs_healthmon		*hm,
	enum xfs_healthmon_type		type,
	enum xfs_healthmon_domain	domain)
{
	struct timespec64		now;
	struct xfs_healthmon_event	*event;

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event) {
		hm->lost_prev_event = true;
		return NULL;
	}

	event->type = type;
	event->domain = domain;
	ktime_get_coarse_real_ts64(&now);
	event->time_ns = (now.tv_sec * NSEC_PER_SEC) + now.tv_nsec;

	return event;
}

/*
 * Before we accept an event notification from a live update hook, we need to
 * clear out any previously lost events.
 */
STATIC int
xfs_healthmon_start_live_update(
	struct xfs_healthmon		*hm)
{
	struct xfs_healthmon_event	*event;

	/* Already unmounted filesystem, do nothing. */
	if (!hm->mp)
		return -ESHUTDOWN;

	/*
	 * If we previously lost an event or the queue is full, try to queue
	 * a notification about lost events.
	 */
	if (!hm->lost_prev_event && hm->events != XFS_HEALTHMON_MAX_EVENTS)
		return 0;

	/*
	 * A previous invocation of the live update hook could not allocate
	 * any memory at all.  If the last event on the list is already a
	 * notification of lost events, we're done.
	 */
	if (hm->last_event && hm->last_event->type == XFS_HEALTHMON_LOST)
		return 0;

	/*
	 * There are no events or the last one wasn't about lost events.  Try
	 * to allocate a new one to note the lost events.
	 */
	event = new_event(hm, XFS_HEALTHMON_LOST, XFS_HEALTHMON_MOUNT);
	if (!event)
		return -ENOMEM;

	hm->lost_prev_event = false;
	xfs_healthmon_push(hm, event);
	return 0;
}

/* Add a shutdown event to the reporting queue. */
STATIC int
xfs_healthmon_shutdown_hook(
	struct notifier_block		*nb,
	unsigned long			action,
	void				*data)
{
	struct xfs_healthmon		*hm;
	struct xfs_healthmon_event	*event;
	int				error;

	hm = container_of(nb, struct xfs_healthmon, shook.shutdown_hook.nb);

	mutex_lock(&hm->lock);

	trace_xfs_healthmon_shutdown_hook(hm->mp, action, hm->events,
			hm->lost_prev_event);

	error = xfs_healthmon_start_live_update(hm);
	if (error)
		goto out_unlock;

	event = new_event(hm, XFS_HEALTHMON_SHUTDOWN, XFS_HEALTHMON_MOUNT);
	if (!event)
		goto out_unlock;

	event->flags = action;
	xfs_healthmon_push(hm, event);

out_unlock:
	mutex_unlock(&hm->lock);
	return NOTIFY_DONE;
}

/* Render the health update type as a string. */
STATIC const char *
xfs_healthmon_typestring(
	const struct xfs_healthmon_event	*event)
{
	static const char *type_strings[] = {
		[XFS_HEALTHMON_LOST]		= "lost",
		[XFS_HEALTHMON_SHUTDOWN]	= "shutdown",
	};

	if (event->type >= ARRAY_SIZE(type_strings))
		return "?";

	return type_strings[event->type];
}

/* Render the health domain as a string. */
STATIC const char *
xfs_healthmon_domstring(
	const struct xfs_healthmon_event	*event)
{
	static const char *dom_strings[] = {
		[XFS_HEALTHMON_MOUNT]		= "mount",
	};

	if (event->domain >= ARRAY_SIZE(dom_strings))
		return "?";

	return dom_strings[event->domain];
}

/* Convert a flags bitmap into a jsonable string. */
static inline int
xfs_healthmon_format_flags(
	struct stdio_redirect		*out,
	const struct flag_string	*strings,
	size_t				nr_strings,
	unsigned int			flags)
{
	const struct flag_string	*p;
	ssize_t				ret;
	unsigned int			i;
	bool				first = true;

	for (i = 0, p = strings; i < nr_strings; i++, p++) {
		if (!(p->mask & flags))
			continue;

		ret = stdio_redirect_printf(out, false, "%s\"%s\"",
				first ? "" : ", ", p->str);
		if (ret < 0)
			return ret;

		first = false;
		flags &= ~p->mask;
	}

	for (i = 0; flags != 0 && i < sizeof(flags) * NBBY; i++) {
		if (!(flags & (1U << i)))
			continue;

		/* json doesn't support hexadecimal notation */
		ret = stdio_redirect_printf(out, false, "%s%u",
				first ? "" : ", ", (1U << i));
		if (ret < 0)
			return ret;

		first = false;
	}

	return 0;
}

/* Convert the event mask into a jsonable string. */
static inline int
__xfs_healthmon_format_mask(
	struct stdio_redirect		*out,
	const char			*descr,
	const struct flag_string	*strings,
	size_t				nr_strings,
	unsigned int			mask)
{
	ssize_t				ret;

	ret = stdio_redirect_printf(out, false, "  \"%s\":  [", descr);
	if (ret < 0)
		return ret;

	ret = xfs_healthmon_format_flags(out, strings, nr_strings, mask);
	if (ret < 0)
		return ret;

	return stdio_redirect_printf(out, false, "],\n");
}

#define xfs_healthmon_format_mask(o, d, s, m) \
	__xfs_healthmon_format_mask((o), (d), (s), ARRAY_SIZE(s), (m))

/* Render shutdown mask as a string set */
static ssize_t
xfs_healthmon_format_shutdown(
	struct stdio_redirect		*out,
	const struct xfs_healthmon_event *event)
{
	static const struct flag_string	mask_strings[] = {
		{ SHUTDOWN_META_IO_ERROR,	"meta_ioerr" },
		{ SHUTDOWN_LOG_IO_ERROR,	"log_ioerr" },
		{ SHUTDOWN_FORCE_UMOUNT,	"force_umount" },
		{ SHUTDOWN_CORRUPT_INCORE,	"corrupt_incore" },
		{ SHUTDOWN_CORRUPT_ONDISK,	"corrupt_ondisk" },
		{ SHUTDOWN_DEVICE_REMOVED,	"device_removed" },
	};

	return xfs_healthmon_format_mask(out, "reasons", mask_strings,
			event->flags);
}

/* Format an event into json. */
STATIC int
xfs_healthmon_format(
	struct xfs_healthmon		*hm,
	const struct xfs_healthmon_event *event)
{
	struct stdio_redirect		*out = &hm->thread.stdio;
	ssize_t				ret;

	ret = stdio_redirect_printf(out, false, "{\n");
	if (ret < 0)
		return ret;

	ret = stdio_redirect_printf(out, false, "  \"type\":       \"%s\",\n",
			xfs_healthmon_typestring(event));
	if (ret < 0)
		return ret;

	ret = stdio_redirect_printf(out, false, "  \"domain\":     \"%s\",\n",
			xfs_healthmon_domstring(event));
	if (ret < 0)
		return ret;

	switch (event->type) {
	case XFS_HEALTHMON_SHUTDOWN:
		ret = xfs_healthmon_format_shutdown(out, event);
		break;
	case XFS_HEALTHMON_LOST:
		/* empty */
		break;
	default:
		break;
	}

	switch (event->domain) {
	case XFS_HEALTHMON_MOUNT:
		/* empty */
		break;
	}
	if (ret < 0)
		return ret;

	trace_xfs_healthmon_format(hm->mp, event);

	/* The last element in the json must not have a trailing comma. */
	ret = stdio_redirect_printf(out, false, "  \"time_ns\":    %llu\n",
			event->time_ns);
	if (ret < 0)
		return ret;

	return stdio_redirect_printf(out, false, "}\n");
}

static inline bool
keep_going(
	struct xfs_healthmon	*hm,
	bool			unmounted)
{
	if (unmounted)
		return false;

	if (kthread_should_stop())
		return false;

	return wait_event_interruptible(hm->wait, hm->events > 0) == 0;
}

/* Pipe health monitoring information to userspace. */
STATIC int
xfs_healthmon_run(
	struct thread_with_stdio	*thr)
{
	struct xfs_healthmon		*hm = to_healthmon(thr);
	struct xfs_healthmon_event	*event;
	bool				unmounted = false;

	while (keep_going(hm, unmounted)) {
		trace_xfs_healthmon_run(hm->mp, hm->events, hm->lost_prev_event);

		mutex_lock(&hm->lock);
		while (!kthread_should_stop() &&
		       (event = xfs_healthmon_pop(hm)) != NULL) {
			mutex_unlock(&hm->lock);

			xfs_healthmon_format(hm, event);
			kfree(event);
			cond_resched();

			mutex_lock(&hm->lock);
		}
		if (!hm->mp)
			unmounted = true;
		mutex_unlock(&hm->lock);
	}

	trace_xfs_healthmon_stop(hm->mp, hm->events, hm->lost_prev_event);

	return 0;
}

/* Validate ioctl parameters. */
static inline bool
xfs_healthmon_validate(
	const struct xfs_health_monitor	*hmo)
{
	if (hmo->flags & ~XFS_HEALTH_MONITOR_ALL)
		return false;
	if (hmo->format != XFS_HEALTH_MONITOR_FMT_JSON)
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
long
xfs_ioc_health_monitor(
	struct xfs_mount		*mp,
	struct xfs_health_monitor __user *arg)
{
	struct xfs_health_monitor	hmo;
	struct xfs_healthmon		*hm;
	int				ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (copy_from_user(&hmo, arg, sizeof(hmo)))
		return -EFAULT;

	if (!xfs_healthmon_validate(&hmo))
		return -EINVAL;

	/*
	 * Health monitor file descriptors maintain a weak reference to the
	 * xfs_mount that they are monitoring.  Hence we must take our own
	 * reference to the driver module to prevent it from being unloaded.
	 */
	if (!try_module_get(THIS_MODULE))
		return -ENOMEM;

	hm = kzalloc(sizeof(*hm), GFP_KERNEL);
	if (!hm) {
		ret = -ENOMEM;
		goto out_mod;
	}

	hm->mp = mp;
	mutex_init(&hm->lock);
	init_waitqueue_head(&hm->wait);

	if (hmo.flags & XFS_HEALTH_MONITOR_VERBOSE)
		hm->verbose = true;

	xfs_shutdown_hook_enable();

	xfs_shutdown_hook_setup(&hm->shook, xfs_healthmon_shutdown_hook);
	ret = xfs_shutdown_hook_add(mp, &hm->shook);
	if (ret)
		goto out_hooks;

	ret = run_thread_with_stdout(&hm->thread, &xfs_healthmon_ops);
	if (ret < 0)
		goto out_shutdown;

	trace_xfs_healthmon_create(mp, hmo.flags, hmo.format);

	return ret;
out_shutdown:
	xfs_shutdown_hook_del(mp, &hm->shook);
out_hooks:
	xfs_shutdown_hook_disable();
	mutex_destroy(&hm->lock);
	xfs_healthmon_free_events(hm);
	kfree(hm);
out_mod:
	module_put(THIS_MODULE);
	return ret;
}
