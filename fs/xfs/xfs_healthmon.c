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
#include "xfs_healthmon.h"

#include <linux/anon_inodes.h>
#include <linux/eventpoll.h>
#include <linux/poll.h>

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
 *	"type": "lost",
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
 */

/* Allow this many events to build up in memory per healthmon fd. */
#define XFS_HEALTHMON_MAX_EVENTS \
		(32768 / sizeof(struct xfs_healthmon_event))

/* Use this much memory for formatting events into user-readable output */
#define XFS_HEALTHMON_BUFSIZE	(1024)

struct flag_string {
	unsigned int	mask;
	const char	*str;
};

struct xfs_healthmon {
	/* lock for mp and eventlist */
	struct mutex			lock;

	/* waiter for signalling the arrival of events */
	struct wait_queue_head		wait;

	/* list of event objects */
	struct xfs_healthmon_event	*first_event;
	struct xfs_healthmon_event	*last_event;

	struct xfs_mount		*mp;

	/* number of events */
	unsigned int			events;

	/* Buffer for formatting events */
	struct seq_buf			outbuf;
	size_t				outpos;

	/* do we want all events? */
	bool				verbose;

	/* did we lose an event? */
	bool				lost_prev_event;
};

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
static inline int
xfs_healthmon_push(
	struct xfs_healthmon		*hm,
	struct xfs_healthmon_event	*event)
{
	/*
	 * If the queue is already full, remember the fact that we lost events.
	 * This doesn't apply to "event lost" events; those always go through
	 * because there should only be one at the very end of the queue.
	 */
	if (hm->events >= XFS_HEALTHMON_MAX_EVENTS &&
	    event->type != XFS_HEALTHMON_LOST) {
		trace_xfs_healthmon_lost_event(hm->mp);
		hm->lost_prev_event = true;
		return -ENOMEM;
	}

	if (!hm->first_event)
		hm->first_event = event;
	if (hm->last_event)
		hm->last_event->next = event;
	hm->last_event = event;
	event->next = NULL;
	hm->events++;
	wake_up(&hm->wait);

	trace_xfs_healthmon_push(hm->mp, event);

	return 0;
}

/* Create a new event or record that we failed. */
static struct xfs_healthmon_event *
xfs_healthmon_alloc(
	struct xfs_healthmon		*hm,
	enum xfs_healthmon_type		type,
	enum xfs_healthmon_domain	domain)
{
	struct timespec64		now;
	struct xfs_healthmon_event	*event;

	event = kzalloc(sizeof(*event), GFP_NOFS);
	if (!event) {
		trace_xfs_healthmon_lost_event(hm->mp);
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
	event = xfs_healthmon_alloc(hm, XFS_HEALTHMON_LOST,
			XFS_HEALTHMON_MOUNT);
	if (!event)
		return -ENOMEM;

	hm->lost_prev_event = false;
	xfs_healthmon_push(hm, event);
	return 0;
}

/* Render the health update type as a string. */
STATIC const char *
xfs_healthmon_typestring(
	const struct xfs_healthmon_event	*event)
{
	static const char *type_strings[] = {
		[XFS_HEALTHMON_LOST]		= "lost",
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
	struct seq_buf			*outbuf,
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

		ret = seq_buf_printf(outbuf, "%s\"%s\"",
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
		ret = seq_buf_printf(outbuf, "%s%u",
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
	struct seq_buf			*outbuf,
	const char			*descr,
	const struct flag_string	*strings,
	size_t				nr_strings,
	unsigned int			mask)
{
	ssize_t				ret;

	ret = seq_buf_printf(outbuf, "  \"%s\":  [", descr);
	if (ret < 0)
		return ret;

	ret = xfs_healthmon_format_flags(outbuf, strings, nr_strings, mask);
	if (ret < 0)
		return ret;

	return seq_buf_printf(outbuf, "],\n");
}

#define xfs_healthmon_format_mask(o, d, s, m) \
	__xfs_healthmon_format_mask((o), (d), (s), ARRAY_SIZE(s), (m))

static inline void
xfs_healthmon_reset_outbuf(
	struct xfs_healthmon		*hm)
{
	hm->outpos = 0;
	seq_buf_clear(&hm->outbuf);
}

/* Format an event into json. */
STATIC int
xfs_healthmon_format(
	struct xfs_healthmon		*hm,
	const struct xfs_healthmon_event *event)
{
	struct seq_buf			*outbuf = &hm->outbuf;
	ssize_t				ret;

	xfs_healthmon_reset_outbuf(hm);

	ret = seq_buf_printf(outbuf, "{\n");
	if (ret < 0)
		goto overrun;

	ret = seq_buf_printf(outbuf, "  \"type\":       \"%s\",\n",
			xfs_healthmon_typestring(event));
	if (ret < 0)
		goto overrun;

	ret = seq_buf_printf(outbuf, "  \"domain\":     \"%s\",\n",
			xfs_healthmon_domstring(event));
	if (ret < 0)
		goto overrun;

	switch (event->type) {
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
		goto overrun;

	trace_xfs_healthmon_format(hm->mp, event);

	/* The last element in the json must not have a trailing comma. */
	ret = seq_buf_printf(outbuf, "  \"time_ns\":    %llu\n",
			event->time_ns);
	if (ret < 0)
		goto overrun;

	ret = seq_buf_printf(outbuf, "}\n");
	if (ret < 0)
		goto overrun;

	ASSERT(!seq_buf_has_overflowed(outbuf));
	return 0;
overrun:
	/*
	 * We overflowed the buffer and could not format the event.  This is
	 * not supposed to happen, but let's clear up the seqbuf, note the lost
	 * event, and bail out.
	 */
	ASSERT(0);
	xfs_healthmon_reset_outbuf(hm);

	trace_xfs_healthmon_lost_event(hm->mp);

	mutex_lock(&hm->lock);
	hm->lost_prev_event = true;
	mutex_unlock(&hm->lock);

	return -EIO;
}

/* How many bytes are waiting in the outbuf to be copied? */
static inline size_t
xfs_healthmon_outbuf_bytes(
	struct xfs_healthmon	*hm)
{
	unsigned int		used = seq_buf_used(&hm->outbuf);

	if (used > hm->outpos)
		return used - hm->outpos;
	return 0;
}

/*
 * Do we have something for userspace to do?  This can mean unmount events,
 * events pending in the queue, or pending bytes in the outbuf.
 */
static inline bool
xfs_healthmon_has_eventdata(
	struct xfs_healthmon	*hm)
{
	return hm->events > 0 || xfs_healthmon_outbuf_bytes(hm) > 0;
}

/* Try to copy the rest of the outbuf to the iov iter. */
STATIC ssize_t
xfs_healthmon_copybuf(
	struct xfs_healthmon	*hm,
	struct iov_iter		*to,
	size_t			to_copy)
{
	size_t			w;

	trace_xfs_healthmon_copybuf(hm->mp, to, &hm->outbuf, hm->outpos);

	w = copy_to_iter(hm->outbuf.buffer + hm->outpos, to_copy, to);
	if (!w)
		return -EFAULT;

	hm->outpos += w;
	return w;
}

/*
 * Convey queued event data to userspace.  First copy any remaining bytes in
 * the outbuf, then format the oldest event into the outbuf and copy that too.
 */
STATIC ssize_t
xfs_healthmon_read_iter(
	struct kiocb		*iocb,
	struct iov_iter		*to)
{
	struct file		*file = iocb->ki_filp;
	struct inode		*inode = file_inode(file);
	struct xfs_healthmon	*hm = file->private_data;
	struct xfs_healthmon_event *event;
	size_t			to_copy, copied = 0;
	ssize_t			ret = 0;

	/* Wait for data to become available */
	if (!(file->f_flags & O_NONBLOCK)) {
		ret = wait_event_interruptible(hm->wait,
				xfs_healthmon_has_eventdata(hm));
		if (ret)
			return ret;
	} else if (!xfs_healthmon_has_eventdata(hm)) {
		return -EAGAIN;
	}

	inode_lock(inode);

	/*
	 * If there's anything left in the seqbuf, copy that before formatting
	 * more events.
	 */
	to_copy = xfs_healthmon_outbuf_bytes(hm);
	if (to_copy > 0) {
		ret = xfs_healthmon_copybuf(hm, to, to_copy);
		if (ret <= 0)
			goto out_unlock;

		copied += ret;
	}

	trace_xfs_healthmon_read(hm->mp, hm->events, hm->lost_prev_event);

	while (iov_iter_count(to) > 0) {
		/*
		 * See if there's an event waiting for us.
		 */
		mutex_lock(&hm->lock);
		event = xfs_healthmon_pop(hm);
		mutex_unlock(&hm->lock);
		if (!event)
			break;

		/* Format the next event into the outbuf. */
		ret = xfs_healthmon_format(hm, event);
		kfree(event);
		if (ret < 0)
			break;

		/* Copy it to userspace */
		to_copy = xfs_healthmon_outbuf_bytes(hm);
		ret = xfs_healthmon_copybuf(hm, to, to_copy);
		if (ret <= 0)
			break;

		copied += ret;
	}

out_unlock:
	inode_unlock(inode);
	return copied ?: ret;
}

/* Poll for available events. */
STATIC __poll_t
xfs_healthmon_poll(
	struct file			*file,
	struct poll_table_struct	*wait)
{
	struct xfs_healthmon		*hm = file->private_data;
	__poll_t			mask = 0;

	poll_wait(file, &hm->wait, wait);

	if (xfs_healthmon_has_eventdata(hm))
		mask |= EPOLLIN;
	return mask;
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
STATIC int
xfs_healthmon_release(
	struct inode		*inode,
	struct file		*file)
{
	struct xfs_healthmon	*hm = file->private_data;

	trace_xfs_healthmon_release(hm->mp, hm->events, hm->lost_prev_event);

	wake_up_all(&hm->wait);

	mutex_destroy(&hm->lock);
	xfs_healthmon_free_events(hm);
	kfree(hm->outbuf.buffer);
	kfree(hm);
	module_put(THIS_MODULE);

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

static const struct file_operations xfs_healthmon_fops = {
	.llseek		= no_llseek,
	.read_iter	= xfs_healthmon_read_iter,
	.poll		= xfs_healthmon_poll,
	.release	= xfs_healthmon_release,
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
	char				*name;
	char				*outbuf;
	struct file			*file = NULL;
	const unsigned int		fd_flags = O_CLOEXEC | O_RDONLY;
	int				fd;
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

	/* Allocate formatting buffer */
	outbuf = kzalloc(XFS_HEALTHMON_BUFSIZE, GFP_KERNEL);
	if (!outbuf) {
		ret = -ENOMEM;
		goto out_hm;
	}
	seq_buf_init(&hm->outbuf, outbuf, XFS_HEALTHMON_BUFSIZE);

	mutex_init(&hm->lock);
	init_waitqueue_head(&hm->wait);

	if (hmo.flags & XFS_HEALTH_MONITOR_VERBOSE)
		hm->verbose = true;

	/* Set up VFS file and file descriptor. */
	ret = get_unused_fd_flags(fd_flags);
	if (ret < 0)
		goto out_mutex;
	fd = ret;

	name = kasprintf(GFP_KERNEL, "XFS (%s): healthmon", mp->m_super->s_id);
	if (!name)
		goto out_fd;

	file = anon_inode_getfile(name, &xfs_healthmon_fops, hm, fd_flags);
	kvfree(name);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto out_fd;
	}

	trace_xfs_healthmon_create(mp, hmo.flags, hmo.format);

	fd_install(fd, file);
	return fd;

out_fd:
	put_unused_fd(fd);
out_mutex:
	mutex_destroy(&hm->lock);
	xfs_healthmon_free_events(hm);
	kfree(hm->outbuf.buffer);
out_hm:
	kfree(hm);
out_mod:
	module_put(THIS_MODULE);
	return ret;
}
