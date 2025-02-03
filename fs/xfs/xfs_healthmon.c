// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024-2025 Oracle.  All Rights Reserved.
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
 */

/* Allow this many events to build up in memory per healthmon fd. */
#define XFS_HEALTHMON_MAX_EVENTS \
		(32768 / sizeof(struct xfs_healthmon_event))

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

	/*
	 * Buffer for formatting events for a read_iter call.  Events are
	 * formatted into the buffer at bufhead, and buftail determines where
	 * to start a copy_iter to get those events to userspace.  All buffer
	 * fields are protected by inode_lock.
	 */
	char				*buffer;
	size_t				bufsize;
	size_t				bufhead;
	size_t				buftail;

	/* XFS_HEALTH_MONITOR_FMT_* */
	uint8_t				format;

	/* do we want all events? */
	bool				verbose;

	/* did we lose previous events? */
	unsigned long long		lost_prev_event;

	/* total counts of events observed and lost events */
	unsigned long long		total_events;
	unsigned long long		total_lost;
};

static inline void xfs_healthmon_bump_events(struct xfs_healthmon *hm)
{
	hm->events++;
	hm->total_events++;
}

static inline void xfs_healthmon_bump_lost(struct xfs_healthmon *hm)
{
	hm->lost_prev_event++;
	hm->total_lost++;
}

/* Remove an event from the head of the list. */
static inline int
xfs_healthmon_free_head(
	struct xfs_healthmon		*hm,
	struct xfs_healthmon_event	*event)
{
	struct xfs_healthmon_event	*head;

	mutex_lock(&hm->lock);
	head = hm->first_event;
	if (head != event) {
		ASSERT(hm->first_event == event);
		mutex_unlock(&hm->lock);
		return -EFSCORRUPTED;
	}

	if (hm->last_event == head)
		hm->last_event = NULL;
	hm->first_event = head->next;
	hm->events--;
	mutex_unlock(&hm->lock);

	trace_xfs_healthmon_pop(hm->mp, head);
	kfree(event);
	return 0;
}

/* Figure out if we can merge two events together. */
static bool
xfs_healthmon_merge_events(
	struct xfs_healthmon_event		*existing,
	const struct xfs_healthmon_event	*new)
{
	if (!existing)
		return false;

	/* type and domain must match to merge events */
	if (existing->type != new->type ||
	    existing->domain != new->domain)
		return false;

	switch (existing->type) {
	case XFS_HEALTHMON_RUNNING:
		/* should only ever be one of these events anyway */
		return false;

	case XFS_HEALTHMON_LOST:
		existing->lostcount += new->lostcount;
		return true;
	}

	return false;
}

/* Push an event onto the end of the queue. */
static inline void
__xfs_healthmon_append(
	struct xfs_healthmon		*hm,
	struct xfs_healthmon_event	*event)
{
	struct timespec64		now;

	ktime_get_coarse_real_ts64(&now);
	event->time_ns = (now.tv_sec * NSEC_PER_SEC) + now.tv_nsec;

	if (!hm->first_event)
		hm->first_event = event;
	if (hm->last_event)
		hm->last_event->next = event;
	hm->last_event = event;
	event->next = NULL;
	xfs_healthmon_bump_events(hm);
	wake_up(&hm->wait);

	trace_xfs_healthmon_push(hm->mp, event);
}

/* Make a stack event dynamic so we can put it on the list. */
static inline struct xfs_healthmon_event *
xfs_healthmon_event_dup(
	const struct xfs_healthmon_event	*event)
{
	return kmemdup(event, sizeof(struct xfs_healthmon_event), GFP_NOFS);
}

/* Deal with any previously lost events */
static int
xfs_healthmon_clear_lost_prev(
	struct xfs_healthmon		*hm)
{
	struct xfs_healthmon_event	lost_event = {
		.type			= XFS_HEALTHMON_LOST,
		.domain			= XFS_HEALTHMON_MOUNT,
		.lostcount		= hm->lost_prev_event,
	};
	struct xfs_healthmon_event	*event;

	if (xfs_healthmon_merge_events(hm->last_event, &lost_event)) {
		trace_xfs_healthmon_merge(hm->mp, hm->last_event);
		wake_up(&hm->wait);
		goto cleared;
	}

	if (hm->events >= XFS_HEALTHMON_MAX_EVENTS)
		return -ENOMEM;

	event = xfs_healthmon_event_dup(&lost_event);
	if (!event)
		return -ENOMEM;

	__xfs_healthmon_append(hm, event);
cleared:
	hm->lost_prev_event = 0;
	return 0;
}

/*
 * Push an event onto the end of the list after dealing with lost events and
 * possibly full queues.
 */
static int
xfs_healthmon_append(
	struct xfs_healthmon			*hm,
	const struct xfs_healthmon_event	*template)
{
	struct xfs_healthmon_event		*event;
	int					error;

	/* Report previously lost events before we do anything else */
	if (hm->lost_prev_event) {
		error = xfs_healthmon_clear_lost_prev(hm);
		if (error)
			return error;
	}

	/* Try to merge with the newest event */
	if (xfs_healthmon_merge_events(hm->last_event, template)) {
		trace_xfs_healthmon_merge(hm->mp, hm->last_event);
		wake_up(&hm->wait);
		return 0;
	}

	/* Already at capacity means we lose the event */
	if (hm->events >= XFS_HEALTHMON_MAX_EVENTS)
		goto lost;

	/* No memory means we lose the event */
	event = xfs_healthmon_event_dup(template);
	if (!event)
		goto lost;

	__xfs_healthmon_append(hm, event);
	return 0;
lost:
	trace_xfs_healthmon_lost_event(hm->mp, hm->lost_prev_event);

	xfs_healthmon_bump_lost(hm);
	return -ENOMEM;
}

static inline void
xfs_healthmon_reset_outbuf(
	struct xfs_healthmon		*hm)
{
	hm->buftail = 0;
	hm->bufhead = 0;
}

static const unsigned int domain_map[] = {
	[XFS_HEALTHMON_MOUNT]		= XFS_HEALTH_MONITOR_DOMAIN_MOUNT,
};

static const unsigned int type_map[] = {
	[XFS_HEALTHMON_RUNNING]		= XFS_HEALTH_MONITOR_TYPE_RUNNING,
	[XFS_HEALTHMON_LOST]		= XFS_HEALTH_MONITOR_TYPE_LOST,
};

/* Copy an event into the output buffer if there's room and advance the head. */
static inline bool
xfs_healthmon_putbuf(
	struct xfs_healthmon			*hm,
	const struct xfs_health_monitor_event	*hme)
{
	if (hm->bufhead + sizeof(*hme) > hm->bufsize)
		return false;

	memcpy(hm->buffer + hm->bufhead, hme, sizeof(*hme));
	hm->bufhead += sizeof(*hme);
	return true;
}

/* Render event as a V0 structure */
STATIC int
xfs_healthmon_format_v0(
	struct xfs_healthmon		*hm,
	const struct xfs_healthmon_event *event)
{
	struct xfs_health_monitor_event	hme = {
		.time_ns		= event->time_ns,
	};

	trace_xfs_healthmon_format(hm->mp, event);

	if (event->domain < 0 || event->domain >= ARRAY_SIZE(domain_map) ||
	    event->type < 0   || event->type >= ARRAY_SIZE(type_map))
		return -EFSCORRUPTED;

	hme.domain = domain_map[event->domain];
	hme.type = type_map[event->type];

	/* fill in the event-specific details */
	switch (event->domain) {
	case XFS_HEALTHMON_MOUNT:
		switch (event->type) {
		case XFS_HEALTHMON_LOST:
			hme.e.lost.count = event->lostcount;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	if (!xfs_healthmon_putbuf(hm, &hme)) {
		/*
		 * We overflowed the buffer and could not format the event.
		 * Tell the caller not to delete the event.
		 */
		trace_xfs_healthmon_format_overflow(hm->mp, event);
		return -1;
	}

	return 0;
}

/* How many bytes are waiting in the outbuf to be copied? */
static inline size_t
xfs_healthmon_outbuf_bytes(
	struct xfs_healthmon	*hm)
{
	if (hm->bufhead > hm->buftail)
		return hm->bufhead - hm->buftail;
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
	struct iov_iter		*to)
{
	size_t			to_copy;
	size_t			w = 0;

	trace_xfs_healthmon_copybuf(hm->mp, to, hm->bufsize, hm->bufhead,
			hm->buftail);

	to_copy = xfs_healthmon_outbuf_bytes(hm);
	if (to_copy) {
		w = copy_to_iter(hm->buffer + hm->buftail, to_copy, to);
		if (!w)
			return -EFAULT;

		hm->buftail += w;
	}

	/*
	 * Nothing left to copy?  Reset the seqbuf pointers and outbuf to the
	 * start since there's no live data in the buffer.
	 */
	if (xfs_healthmon_outbuf_bytes(hm) == 0)
		xfs_healthmon_reset_outbuf(hm);
	return w;
}

/*
 * See if there's an event waiting for us.  If the fs is no longer mounted,
 * don't bother sending any more events.
 */
static inline struct xfs_healthmon_event *
xfs_healthmon_peek(
	struct xfs_healthmon	*hm)
{
	struct xfs_healthmon_event *event;

	mutex_lock(&hm->lock);
	if (hm->mp)
		event = hm->first_event;
	else
		event = NULL;
	mutex_unlock(&hm->lock);
	return event;
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
	size_t			copied = 0;
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

	/* Allocate formatting buffer up to 64k if necessary */
	if (hm->bufsize == 0) {
		void		*outbuf;
		size_t		bufsize = min(65536, max(PAGE_SIZE,
							 iov_iter_count(to)));

		outbuf = kzalloc(bufsize, GFP_KERNEL);
		if (!outbuf) {
			bufsize = PAGE_SIZE;
			outbuf = kzalloc(bufsize, GFP_KERNEL);
			if (!outbuf)
				return -ENOMEM;
		}

		inode_lock(inode);
		if (hm->bufsize == 0) {
			hm->buffer = outbuf;
			hm->bufsize = bufsize;
			hm->bufhead = 0;
			hm->buftail = 0;
		} else {
			kfree(outbuf);
		}
	} else {
		inode_lock(inode);
	}

	trace_xfs_healthmon_read_start(hm->mp, hm->events, hm->lost_prev_event);

	/*
	 * If there's anything left in the seqbuf, copy that before formatting
	 * more events.
	 */
	ret = xfs_healthmon_copybuf(hm, to);
	if (ret < 0)
		goto out_unlock;
	copied += ret;

	while (iov_iter_count(to) > 0) {
		/* Format the next events into the outbuf until it's full. */
		while ((event = xfs_healthmon_peek(hm)) != NULL) {
			switch (hm->format) {
			case XFS_HEALTH_MONITOR_FMT_V0:
				ret = xfs_healthmon_format_v0(hm, event);
				break;
			default:
				ret = -EINVAL;
				goto out_unlock;
			}
			if (ret < 0)
				break;
			ret = xfs_healthmon_free_head(hm, event);
			if (ret)
				goto out_unlock;
		}

		/* Copy it to userspace */
		ret = xfs_healthmon_copybuf(hm, to);
		if (ret <= 0)
			break;

		copied += ret;
	}

out_unlock:
	trace_xfs_healthmon_read_finish(hm->mp, hm->events, hm->lost_prev_event);
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
	kfree(hm->buffer);
	kfree(hm);

	return 0;
}

/* Validate ioctl parameters. */
static inline bool
xfs_healthmon_validate(
	const struct xfs_health_monitor	*hmo)
{
	if (hmo->flags & ~XFS_HEALTH_MONITOR_ALL)
		return false;
	if (hmo->format != XFS_HEALTH_MONITOR_FMT_V0)
		return false;
	if (memchr_inv(&hmo->pad1, 0, sizeof(hmo->pad1)))
		return false;
	if (memchr_inv(&hmo->pad2, 0, sizeof(hmo->pad2)))
		return false;
	return true;
}

/* Emit some data about the health monitoring fd. */
#ifdef CONFIG_PROC_FS
static const char *
xfs_healthmon_format_string(const struct xfs_healthmon *hm)
{
	switch (hm->format) {
	case XFS_HEALTH_MONITOR_FMT_V0:
		return "v0";
	}

	return "";
}

static void
xfs_healthmon_show_fdinfo(
	struct seq_file		*m,
	struct file		*file)
{
	struct xfs_healthmon	*hm = file->private_data;

	mutex_lock(&hm->lock);
	seq_printf(m, "state:\talive\ndev:\t%s\nformat:\t%s\nevents:\t%llu\nlost:\t%llu\n",
			hm->mp->m_super->s_id,
			xfs_healthmon_format_string(hm),
			hm->total_events,
			hm->total_lost);
	mutex_unlock(&hm->lock);
}
#endif

static const struct file_operations xfs_healthmon_fops = {
	.owner		= THIS_MODULE,
#ifdef CONFIG_PROC_FS
	.show_fdinfo	= xfs_healthmon_show_fdinfo,
#endif
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
	struct xfs_healthmon_event	running_event = {
		.type			= XFS_HEALTHMON_RUNNING,
		.domain			= XFS_HEALTHMON_MOUNT,
	};
	struct xfs_health_monitor	hmo;
	struct xfs_healthmon		*hm;
	int				fd;
	int				ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (copy_from_user(&hmo, arg, sizeof(hmo)))
		return -EFAULT;

	if (!xfs_healthmon_validate(&hmo))
		return -EINVAL;

	hm = kzalloc(sizeof(*hm), GFP_KERNEL);
	if (!hm)
		return -ENOMEM;
	hm->mp = mp;
	hm->format = hmo.format;

	mutex_init(&hm->lock);
	init_waitqueue_head(&hm->wait);

	if (hmo.flags & XFS_HEALTH_MONITOR_VERBOSE)
		hm->verbose = true;

	/* Queue up the first event that lets the client know we're running. */
	ret = xfs_healthmon_append(hm, &running_event);
	if (ret)
		goto out_mutex;

	/*
	 * Create the anonymous file.  If it succeeds, the file owns hm and
	 * can go away at any time, so we must not access it again.
	 */
	fd = anon_inode_getfd("xfs_healthmon", &xfs_healthmon_fops, hm,
			O_CLOEXEC | O_RDONLY);
	if (fd < 0) {
		ret = fd;
		goto out_mutex;
	}

	trace_xfs_healthmon_create(mp, hmo.flags, hmo.format);

	return fd;

out_mutex:
	mutex_destroy(&hm->lock);
	xfs_healthmon_free_events(hm);
	kfree(hm);
	return ret;
}
