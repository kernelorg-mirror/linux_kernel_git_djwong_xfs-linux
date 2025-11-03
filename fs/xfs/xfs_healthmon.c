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
#include "xfs_health.h"
#include "xfs_healthmon.h"
#include "xfs_fsops.h"
#include "xfs_notify_failure.h"
#include "xfs_file.h"
#include "xfs_ioctl.h"

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

	/* charge event object usage to this memory cgroup */
	struct mem_cgroup		*memcg;

	/* live update hooks */
	struct xfs_shutdown_hook	shook;
	struct xfs_health_hook		hhook;
	struct xfs_media_error_hook	mhook;
	struct fs_error_hook		fhook;

	/* filesystem mount, or NULL if we've unmounted */
	struct xfs_mount		*mp;

	/* filesystem type for safe cleanup of hooks; requires module_get */
	struct file_system_type		*fstyp;

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
	case XFS_HEALTHMON_UNMOUNT:
		/* should only ever be one of these events anyway */
		return false;

	case XFS_HEALTHMON_LOST:
		existing->lostcount += new->lostcount;
		return true;

	case XFS_HEALTHMON_SICK:
	case XFS_HEALTHMON_CORRUPT:
	case XFS_HEALTHMON_HEALTHY:
		switch (existing->domain) {
		case XFS_HEALTHMON_FS:
			existing->fsmask |= new->fsmask;
			return true;
		case XFS_HEALTHMON_AG:
		case XFS_HEALTHMON_RTGROUP:
			if (existing->group == new->group){
				existing->grpmask |= new->grpmask;
				return true;
			}
			return false;
		case XFS_HEALTHMON_INODE:
			if (existing->ino == new->ino &&
			    existing->gen == new->gen) {
				existing->imask |= new->imask;
				return true;
			}
			return false;
		default:
			ASSERT(0);
			return false;
		}
		return false;

	case XFS_HEALTHMON_SHUTDOWN:
		/* yes, we can race to shutdown */
		existing->flags |= new->flags;
		return true;

	case XFS_HEALTHMON_MEDIA_ERROR:
		/* physically adjacent errors can merge */
		if (existing->daddr + existing->bbcount == new->daddr) {
			existing->bbcount += new->bbcount;
			return true;
		}
		if (new->daddr + new->bbcount == existing->daddr) {
			existing->daddr = new->daddr;
			existing->bbcount += new->bbcount;
			return true;
		}
		return false;

	case XFS_HEALTHMON_BUFREAD:
	case XFS_HEALTHMON_BUFWRITE:
	case XFS_HEALTHMON_DIOREAD:
	case XFS_HEALTHMON_DIOWRITE:
	case XFS_HEALTHMON_DATALOST:
		/* logically adjacent file ranges can merge */
		if (existing->fino != new->fino || existing->fgen != new->fgen)
			return false;

		if (existing->fpos + existing->flen == new->fpos) {
			existing->flen += new->flen;
			return true;
		}

		if (new->fpos + new->flen == existing->fpos) {
			existing->fpos = new->fpos;
			existing->flen += new->flen;
			return true;
		}
		return false;
	}

	return false;
}

/* Insert an event onto the start of the queue. */
static inline void
__xfs_healthmon_insert(
	struct xfs_healthmon		*hm,
	struct xfs_healthmon_event	*event)
{
	struct timespec64		now;

	ktime_get_coarse_real_ts64(&now);
	event->time_ns = (now.tv_sec * NSEC_PER_SEC) + now.tv_nsec;

	event->next = hm->first_event;
	if (!hm->first_event)
		hm->first_event = event;
	if (!hm->last_event)
		hm->last_event = event;
	xfs_healthmon_bump_events(hm);
	wake_up(&hm->wait);

	trace_xfs_healthmon_insert(hm->mp, event);
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

	/* Filesystem already unmounted, do nothing. */
	if (!hm->mp)
		return -ESHUTDOWN;

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

static inline enum xfs_healthmon_type
health_update_to_type(
	enum xfs_health_update_type	type)
{
	switch (type) {
	case XFS_HEALTHUP_SICK:
		return XFS_HEALTHMON_SICK;
	case XFS_HEALTHUP_CORRUPT:
		return XFS_HEALTHMON_CORRUPT;
	case XFS_HEALTHUP_HEALTHY:
		return XFS_HEALTHMON_HEALTHY;
	case XFS_HEALTHUP_UNMOUNT:
		/* static checking */
		break;
	}
	return XFS_HEALTHMON_UNMOUNT;
}

static inline enum xfs_healthmon_domain
health_update_to_domain(
	enum xfs_health_update_domain	domain)
{
	switch (domain) {
	case XFS_HEALTHUP_FS:
		return XFS_HEALTHMON_FS;
	case XFS_HEALTHUP_AG:
		return XFS_HEALTHMON_AG;
	case XFS_HEALTHUP_RTGROUP:
		return XFS_HEALTHMON_RTGROUP;
	case XFS_HEALTHUP_INODE:
		/* static checking */
		break;
	}
	return XFS_HEALTHMON_INODE;
}

/* Deal with an unmount event. */
STATIC int
xfs_healthmon_unmount(
	struct xfs_healthmon		*hm)
{
	struct xfs_healthmon_event	*event;
	struct mem_cgroup		*old_memcg;

	mutex_lock(&hm->lock);

	trace_xfs_healthmon_unmount(hm->mp, hm->events, hm->lost_prev_event);

	old_memcg = set_active_memcg(hm->memcg);
	event = kzalloc(sizeof(struct xfs_healthmon_event), GFP_NOFS);
	set_active_memcg(old_memcg);
	if (event) {
		/*
		 * Insert the unmount notification at the start of the event
		 * queue so that userspace knows the filesystem went away as
		 * soon as possible.  There's nothing actionable for userspace
		 * after an unmount.
		 */
		event->type = XFS_HEALTHMON_UNMOUNT;
		event->domain = XFS_HEALTHMON_MOUNT;

		__xfs_healthmon_insert(hm, event);
	} else {
		/*
		 * Wake up the reader directly in case we didn't have enough
		 * memory to queue the unmount event.  The filesystem is about
		 * to go away so we don't care about reporting previously lost
		 * events.
		 */
		wake_up(&hm->wait);
	}

	/*
	 * The filesystem is unmounting, so we must detach from the mount.
	 * After this function returns, the healthmon thread has no connection
	 * to the mounted filesystem and must not touch its hooks.
	 */
	hm->mp = NULL;
	mutex_unlock(&hm->lock);
	return NOTIFY_DONE;
}

/* Compute the reporting mask for non-unmount metadata health events. */
static inline unsigned int
xfs_healthmon_event_mask(
	struct xfs_healthmon			*hm,
	unsigned long				action,
	const struct xfs_health_update_params	*hup)
{
	/* If we want all events, return all events. */
	if (hm->verbose)
		return hup->new_mask;

	switch (action) {
	case XFS_HEALTHUP_SICK:
		/* Always report runtime corruptions */
		return hup->new_mask;
	case XFS_HEALTHUP_CORRUPT:
		/* Only report new fsck errors */
		return hup->new_mask & ~hup->old_mask;
	case XFS_HEALTHUP_HEALTHY:
		/* Only report healthy metadata that got fixed */
		return hup->new_mask & hup->old_mask;
	}

	ASSERT(0);
	return 0;
}

/* Handle a metadata event */
STATIC int
xfs_healthmon_metadata(
	struct xfs_healthmon		*hm,
	unsigned long			action,
	struct xfs_health_update_params	*hup,
	unsigned int			mask)
{
	struct xfs_healthmon_event	event = {
		.type			= health_update_to_type(action),
		.domain			= health_update_to_domain(hup->domain),
	};
	struct mem_cgroup		*old_memcg;

	mutex_lock(&hm->lock);

	trace_xfs_healthmon_metadata_hook(hm->mp, action, hup, hm->events,
			hm->lost_prev_event);

	/* Ignore the event if it's only reporting a secondary health state. */
	switch (event.domain) {
	case XFS_HEALTHMON_FS:
		event.fsmask = mask & ~XFS_SICK_FS_SECONDARY;
		if (!event.fsmask)
			goto out_unlock;
		break;
	case XFS_HEALTHMON_AG:
		event.grpmask = mask & ~XFS_SICK_AG_SECONDARY;
		if (!event.grpmask)
			goto out_unlock;
		event.group = hup->group;
		break;
	case XFS_HEALTHMON_RTGROUP:
		event.grpmask = mask & ~XFS_SICK_RG_SECONDARY;
		if (!event.grpmask)
			goto out_unlock;
		event.group = hup->group;
		break;
	case XFS_HEALTHMON_INODE:
		event.imask = mask & ~XFS_SICK_INO_SECONDARY;
		if (!event.imask)
			goto out_unlock;
		event.ino = hup->ino;
		event.gen = hup->gen;
		break;
	default:
		ASSERT(0);
		goto out_unlock;
	}

	old_memcg = set_active_memcg(hm->memcg);
	xfs_healthmon_append(hm, &event);
	set_active_memcg(old_memcg);
out_unlock:
	mutex_unlock(&hm->lock);
	return NOTIFY_DONE;
}

/* Add a health event to the reporting queue. */
STATIC int
xfs_healthmon_metadata_hook(
	struct notifier_block		*nb,
	unsigned long			action,
	void				*data)
{
	struct xfs_health_update_params	*hup = data;
	struct xfs_healthmon		*hm =
		container_of(nb, struct xfs_healthmon, hhook.health_hook.nb);
	unsigned int			mask = 0;

	if (action == XFS_HEALTHUP_UNMOUNT)
		return xfs_healthmon_unmount(hm);

	mask = xfs_healthmon_event_mask(hm, action, hup);
	if (mask)
		return xfs_healthmon_metadata(hm, action, hup, mask);

	return NOTIFY_DONE;
}

/* Add a shutdown event to the reporting queue. */
STATIC int
xfs_healthmon_shutdown_hook(
	struct notifier_block		*nb,
	unsigned long			action,
	void				*data)
{
	struct xfs_healthmon_event	event = {
		.type			= XFS_HEALTHMON_SHUTDOWN,
		.domain			= XFS_HEALTHMON_MOUNT,
		.flags			= action,
	};
	struct xfs_healthmon		*hm =
		container_of(nb, struct xfs_healthmon, shook.shutdown_hook.nb);
	struct mem_cgroup		*old_memcg;

	mutex_lock(&hm->lock);

	trace_xfs_healthmon_shutdown_hook(hm->mp, action, hm->events,
			hm->lost_prev_event);

	old_memcg = set_active_memcg(hm->memcg);
	xfs_healthmon_append(hm, &event);
	set_active_memcg(old_memcg);

	mutex_unlock(&hm->lock);
	return NOTIFY_DONE;
}

static inline enum xfs_healthmon_domain
media_error_domain(
	const struct xfs_media_error_params	*p)
{
	switch (p->fdev) {
	case XFS_FAILED_LOGDEV:
		return XFS_HEALTHMON_LOGDEV;
	case XFS_FAILED_RTDEV:
		return XFS_HEALTHMON_RTDEV;
	case XFS_FAILED_DATADEV:
		return XFS_HEALTHMON_DATADEV;
	}

	/* shut up gcc */
	return 0;
}

/* Add a media error event to the reporting queue. */
STATIC int
xfs_healthmon_media_error_hook(
	struct notifier_block		*nb,
	unsigned long			action,
	void				*data)
{
	struct xfs_media_error_params	*p = data;
	struct xfs_healthmon_event	event = {
		.type			= XFS_HEALTHMON_MEDIA_ERROR,
		.domain			= media_error_domain(p),
		.daddr			= p->daddr,
		.bbcount		= p->bbcount,
	};
	struct xfs_healthmon		*hm =
		container_of(nb, struct xfs_healthmon, mhook.error_hook.nb);
	struct mem_cgroup		*old_memcg;

	mutex_lock(&hm->lock);

	trace_xfs_healthmon_media_error_hook(p, hm->events,
			hm->lost_prev_event);

	old_memcg = set_active_memcg(hm->memcg);
	xfs_healthmon_append(hm, &event);
	set_active_memcg(old_memcg);

	mutex_unlock(&hm->lock);
	return NOTIFY_DONE;
}

static inline enum xfs_healthmon_type file_ioerr_type(unsigned long action)
{
	switch (action) {
	case FSERR_BUFFERED_READ:
		return XFS_HEALTHMON_BUFREAD;
	case FSERR_BUFFERED_WRITE:
		return XFS_HEALTHMON_BUFWRITE;
	case FSERR_DIRECTIO_READ:
		return XFS_HEALTHMON_DIOREAD;
	case FSERR_DIRECTIO_WRITE:
		return XFS_HEALTHMON_DIOWRITE;
	case FSERR_DATA_LOST:
		return XFS_HEALTHMON_DATALOST;
	}

	ASSERT(0);
	return -1;
}

/* Add a file io error event to the reporting queue. */
STATIC int
xfs_healthmon_file_ioerror_hook(
	struct notifier_block		*nb,
	unsigned long			action,
	void				*data)
{
	struct fs_error			*p = data;
	struct xfs_inode		*ip = XFS_I(p->inode);
	struct xfs_healthmon_event	event = {
		.type			= file_ioerr_type(action),
		.domain			= XFS_HEALTHMON_FILERANGE,
		.fino			= ip->i_ino,
		.fgen			= VFS_I(ip)->i_generation,
		.fpos			= p->pos,
		.flen			= p->len,
	};
	struct xfs_healthmon		*hm =
		container_of(nb, struct xfs_healthmon, fhook.nb);
	struct mem_cgroup		*old_memcg;

	if (event.type == -1)
		return NOTIFY_DONE;

	mutex_lock(&hm->lock);

	trace_xfs_healthmon_file_ioerror_hook(hm->mp, action, p, hm->events,
			hm->lost_prev_event);

	old_memcg = set_active_memcg(hm->memcg);
	xfs_healthmon_append(hm, &event);
	set_active_memcg(old_memcg);

	mutex_unlock(&hm->lock);
	return NOTIFY_DONE;
}

static inline void
xfs_healthmon_reset_outbuf(
	struct xfs_healthmon		*hm)
{
	hm->buftail = 0;
	hm->bufhead = 0;
}

struct flags_map {
	unsigned int		in_mask;
	unsigned int		out_mask;
};

static const struct flags_map shutdown_map[] = {
	{ SHUTDOWN_META_IO_ERROR,	XFS_HEALTH_SHUTDOWN_META_IO_ERROR },
	{ SHUTDOWN_LOG_IO_ERROR,	XFS_HEALTH_SHUTDOWN_LOG_IO_ERROR },
	{ SHUTDOWN_FORCE_UMOUNT,	XFS_HEALTH_SHUTDOWN_FORCE_UMOUNT },
	{ SHUTDOWN_CORRUPT_INCORE,	XFS_HEALTH_SHUTDOWN_CORRUPT_INCORE },
	{ SHUTDOWN_CORRUPT_ONDISK,	XFS_HEALTH_SHUTDOWN_CORRUPT_ONDISK },
	{ SHUTDOWN_DEVICE_REMOVED,	XFS_HEALTH_SHUTDOWN_DEVICE_REMOVED },
};

static inline unsigned int
__map_flags(
	const struct flags_map	*map,
	size_t			array_len,
	unsigned int		flags)
{
	const struct flags_map	*m;
	unsigned int		ret = 0;

	for (m = map; m < map + array_len; m++) {
		if (flags & m->in_mask)
			ret |= m->out_mask;
	}

	return ret;
}

#define map_flags(map, flags) __map_flags((map), ARRAY_SIZE(map), (flags))

static inline unsigned int shutdown_mask(unsigned int in)
{
	return map_flags(shutdown_map, in);
}

static const unsigned int domain_map[] = {
	[XFS_HEALTHMON_MOUNT]		= XFS_HEALTH_MONITOR_DOMAIN_MOUNT,
	[XFS_HEALTHMON_FS]		= XFS_HEALTH_MONITOR_DOMAIN_FS,
	[XFS_HEALTHMON_AG]		= XFS_HEALTH_MONITOR_DOMAIN_AG,
	[XFS_HEALTHMON_INODE]		= XFS_HEALTH_MONITOR_DOMAIN_INODE,
	[XFS_HEALTHMON_RTGROUP]		= XFS_HEALTH_MONITOR_DOMAIN_RTGROUP,
	[XFS_HEALTHMON_DATADEV]		= XFS_HEALTH_MONITOR_DOMAIN_DATADEV,
	[XFS_HEALTHMON_RTDEV]		= XFS_HEALTH_MONITOR_DOMAIN_RTDEV,
	[XFS_HEALTHMON_LOGDEV]		= XFS_HEALTH_MONITOR_DOMAIN_LOGDEV,
	[XFS_HEALTHMON_FILERANGE]	= XFS_HEALTH_MONITOR_DOMAIN_FILERANGE,
};

static const unsigned int type_map[] = {
	[XFS_HEALTHMON_RUNNING]		= XFS_HEALTH_MONITOR_TYPE_RUNNING,
	[XFS_HEALTHMON_LOST]		= XFS_HEALTH_MONITOR_TYPE_LOST,
	[XFS_HEALTHMON_SICK]		= XFS_HEALTH_MONITOR_TYPE_SICK,
	[XFS_HEALTHMON_CORRUPT]		= XFS_HEALTH_MONITOR_TYPE_CORRUPT,
	[XFS_HEALTHMON_HEALTHY]		= XFS_HEALTH_MONITOR_TYPE_HEALTHY,
	[XFS_HEALTHMON_UNMOUNT]		= XFS_HEALTH_MONITOR_TYPE_UNMOUNT,
	[XFS_HEALTHMON_SHUTDOWN]	= XFS_HEALTH_MONITOR_TYPE_SHUTDOWN,
	[XFS_HEALTHMON_MEDIA_ERROR]	= XFS_HEALTH_MONITOR_TYPE_MEDIA_ERROR,
	[XFS_HEALTHMON_BUFREAD]		= XFS_HEALTH_MONITOR_TYPE_BUFREAD,
	[XFS_HEALTHMON_BUFWRITE]	= XFS_HEALTH_MONITOR_TYPE_BUFWRITE,
	[XFS_HEALTHMON_DIOREAD]		= XFS_HEALTH_MONITOR_TYPE_DIOREAD,
	[XFS_HEALTHMON_DIOWRITE]	= XFS_HEALTH_MONITOR_TYPE_DIOWRITE,
	[XFS_HEALTHMON_DATALOST]	= XFS_HEALTH_MONITOR_TYPE_DATALOST,
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
		case XFS_HEALTHMON_SHUTDOWN:
			hme.e.shutdown.reasons = shutdown_mask(event->flags);
			break;
		default:
			break;
		}
		break;
	case XFS_HEALTHMON_FS:
		hme.e.fs.mask = xfs_healthmon_fs_mask(event->fsmask);
		break;
	case XFS_HEALTHMON_RTGROUP:
		hme.e.group.mask = xfs_healthmon_rtgroup_mask(event->grpmask);
		hme.e.group.gno = event->group;
		break;
	case XFS_HEALTHMON_AG:
		hme.e.group.mask = xfs_healthmon_perag_mask(event->grpmask);
		hme.e.group.gno = event->group;
		break;
	case XFS_HEALTHMON_INODE:
		hme.e.inode.mask = xfs_healthmon_inode_mask(event->imask);
		hme.e.inode.ino = event->ino;
		hme.e.inode.gen = event->gen;
		break;
	case XFS_HEALTHMON_DATADEV:
	case XFS_HEALTHMON_LOGDEV:
	case XFS_HEALTHMON_RTDEV:
		hme.e.media.daddr = event->daddr;
		hme.e.media.bbcount = event->bbcount;
		break;
	case XFS_HEALTHMON_FILERANGE:
		hme.e.filerange.ino = event->fino;
		hme.e.filerange.gen = event->fgen;
		hme.e.filerange.pos = event->fpos;
		hme.e.filerange.len = event->flen;
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
	return !hm->mp || hm->events > 0 || xfs_healthmon_outbuf_bytes(hm) > 0;
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
	event = hm->first_event;
	if (hm->mp)
		goto done;

	/* If the filesystem is unmounted, only return the unmount event */
	if (event && event->type == XFS_HEALTHMON_UNMOUNT)
		goto done;
	event = NULL;

done:
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

/*
 * Detach all filesystem hooks that were set up for a health monitor.  Only
 * call this from iterate_super*.
 */
STATIC void
xfs_healthmon_detach_hooks(
	struct super_block	*sb,
	void			*arg)
{
	struct xfs_healthmon	*hm = arg;

	mutex_lock(&hm->lock);

	/*
	 * Because health monitors have a weak reference to the filesystem
	 * they're monitoring, the hook deletions below must not race against
	 * that filesystem being unmounted because that could lead to UAF
	 * errors.
	 *
	 * If hm->mp is NULL, the health unmount hook already ran and the hook
	 * chain head (contained within the xfs_mount structure) is gone.  Do
	 * not detach any hooks; just let them get freed when the healthmon
	 * object is torn down.
	 */
	if (!hm->mp)
		goto out_unlock;

	/*
	 * Otherwise, the caller gave us a non-dying @sb with s_umount held in
	 * shared mode, which means that @sb cannot be running through
	 * deactivate_locked_super and cannot be freed.  It's safe to compare
	 * @sb against the super that we snapshotted when we set up the health
	 * monitor.
	 */
	if (hm->mp->m_super != sb)
		goto out_unlock;

	mutex_unlock(&hm->lock);

	/*
	 * Now we know that the filesystem @hm->mp is active and cannot be
	 * deactivated until this function returns.  Unmount events are sent
	 * through the health monitoring subsystem from xfs_fs_put_super, so
	 * it is now time to detach the hooks.
	 */
	sb_unhook_error(hm->mp->m_super, &hm->fhook);
	xfs_media_error_hook_del(hm->mp, &hm->mhook);
	xfs_shutdown_hook_del(hm->mp, &hm->shook);
	xfs_health_hook_del(hm->mp, &hm->hhook);
	return;

out_unlock:
	mutex_unlock(&hm->lock);
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

	iterate_supers_type(hm->fstyp, xfs_healthmon_detach_hooks, hm);
	xfs_health_hook_disable();

	mutex_destroy(&hm->lock);
	xfs_healthmon_free_events(hm);
	kfree(hm->buffer);
	mem_cgroup_put(hm->memcg);
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
	if (!hm->mp) {
		seq_printf(m, "state:\tdead\n");
		goto out_unlock;
	}

	seq_printf(m, "state:\talive\ndev:\t%s\nformat:\t%s\nevents:\t%llu\nlost:\t%llu\n",
			hm->mp->m_super->s_id,
			xfs_healthmon_format_string(hm),
			hm->total_events,
			hm->total_lost);

out_unlock:
	mutex_unlock(&hm->lock);
}
#endif

/* Reconfigure the health monitor. */
STATIC long
xfs_healthmon_reconfigure(
	struct file			*file,
	unsigned int			cmd,
	void __user			*arg)
{
	struct xfs_health_monitor	hmo;
	struct xfs_healthmon		*hm = file->private_data;
	int				error = 0;

	if (copy_from_user(&hmo, arg, sizeof(hmo)))
		return -EFAULT;

	if (!xfs_healthmon_validate(&hmo))
		return -EINVAL;

	mutex_lock(&hm->lock);
	if (hm->format != hmo.format && xfs_healthmon_outbuf_bytes(hm) > 0) {
		error = -EBUSY;
		goto out_unlock;
	}

	hm->format = hmo.format;
	hm->verbose = !!(hmo.flags & XFS_HEALTH_MONITOR_VERBOSE);

out_unlock:
	mutex_unlock(&hm->lock);
	return error;
}

/* Does the fd point to the same filesystem as the one we're monitoring? */
STATIC long
xfs_healthmon_samefs(
	struct file			*file,
	unsigned int			cmd,
	void __user			*arg)
{
	struct xfs_health_samefs	hms;
	struct xfs_healthmon		*hm = file->private_data;
	struct inode			*hms_inode;
	int				ret = 0;

	if (copy_from_user(&hms, arg, sizeof(hms)))
		return -EFAULT;

	if (hms.flags)
		return -EINVAL;

	CLASS(fd, hms_fd)(hms.fd);
	if (fd_empty(hms_fd))
		return -EBADF;

	hms_inode = file_inode(fd_file(hms_fd));
	mutex_lock(&hm->lock);
	if (!hm->mp || hm->mp->m_super != hms_inode->i_sb)
		ret = -ESTALE;
	mutex_unlock(&hm->lock);
	return ret;
}

/* Handle ioctls for the health monitoring thread. */
STATIC long
xfs_healthmon_ioctl(
	struct file			*file,
	unsigned int			cmd,
	unsigned long			p)
{
	void __user			*arg = (void __user *)p;

	switch (cmd) {
	case XFS_IOC_HEALTH_MONITOR:
		return xfs_healthmon_reconfigure(file, cmd, arg);
	case XFS_IOC_HEALTH_SAMEFS:
		return xfs_healthmon_samefs(file, cmd, arg);
	default:
		break;
	}

	return -ENOTTY;
}

static const struct file_operations xfs_healthmon_fops = {
	.owner		= THIS_MODULE,
#ifdef CONFIG_PROC_FS
	.show_fdinfo	= xfs_healthmon_show_fdinfo,
#endif
	.read_iter	= xfs_healthmon_read_iter,
	.poll		= xfs_healthmon_poll,
	.release	= xfs_healthmon_release,
	.unlocked_ioctl	= xfs_healthmon_ioctl,
};

/*
 * Create a health monitoring file.  Returns an index to the fd table or a
 * negative errno.
 */
long
xfs_ioc_health_monitor(
	struct file			*file,
	struct xfs_health_monitor __user *arg)
{
	struct xfs_healthmon_event	running_event = {
		.type			= XFS_HEALTHMON_RUNNING,
		.domain			= XFS_HEALTHMON_MOUNT,
	};
	struct xfs_health_monitor	hmo;
	struct xfs_healthmon		*hm;
	struct xfs_inode		*ip = XFS_I(file_inode(file));
	struct xfs_mount		*mp = ip->i_mount;
	int				fd;
	int				ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (ip->i_ino != mp->m_sb.sb_rootino)
		return -EPERM;
	if (current_user_ns() != &init_user_ns)
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
	hm->memcg = get_mem_cgroup_from_mm(current->mm);

	/*
	 * Since we already got a ref to the module, take a reference to the
	 * fstype to make it easier to detach the hooks when we tear things
	 * down later.
	 */
	hm->fstyp = mp->m_super->s_type;

	mutex_init(&hm->lock);
	init_waitqueue_head(&hm->wait);

	if (hmo.flags & XFS_HEALTH_MONITOR_VERBOSE)
		hm->verbose = true;

	/* Enable hooks to receive events, generally. */
	xfs_health_hook_enable();

	/* Attach specific event hooks to this monitor. */
	xfs_health_hook_setup(&hm->hhook, xfs_healthmon_metadata_hook);
	ret = xfs_health_hook_add(mp, &hm->hhook);
	if (ret)
		goto out_hooks;

	xfs_shutdown_hook_setup(&hm->shook, xfs_healthmon_shutdown_hook);
	ret = xfs_shutdown_hook_add(mp, &hm->shook);
	if (ret)
		goto out_healthhook;

	xfs_media_error_hook_setup(&hm->mhook, xfs_healthmon_media_error_hook);
	ret = xfs_media_error_hook_add(mp, &hm->mhook);
	if (ret)
		goto out_shutdownhook;

	sb_init_error_hook(&hm->fhook, xfs_healthmon_file_ioerror_hook);
	ret = sb_hook_error(mp->m_super, &hm->fhook);
	if (ret)
		goto out_mediahook;

	/* Queue up the first event that lets the client know we're running. */
	ret = xfs_healthmon_append(hm, &running_event);
	if (ret)
		goto out_ioerrhook;

	/*
	 * Create the anonymous file.  If it succeeds, the file owns hm and
	 * can go away at any time, so we must not access it again.
	 */
	fd = anon_inode_getfd("xfs_healthmon", &xfs_healthmon_fops, hm,
			O_CLOEXEC | O_RDONLY);
	if (fd < 0) {
		ret = fd;
		goto out_ioerrhook;
	}

	trace_xfs_healthmon_create(mp, hmo.flags, hmo.format);

	return fd;

out_ioerrhook:
	sb_unhook_error(mp->m_super, &hm->fhook);
out_mediahook:
	xfs_media_error_hook_del(mp, &hm->mhook);
out_shutdownhook:
	xfs_shutdown_hook_del(mp, &hm->shook);
out_healthhook:
	xfs_health_hook_del(mp, &hm->hhook);
out_hooks:
	xfs_health_hook_disable();
	mutex_destroy(&hm->lock);
	xfs_healthmon_free_events(hm);
	mem_cgroup_put(hm->memcg);
	kfree(hm);
	return ret;
}
