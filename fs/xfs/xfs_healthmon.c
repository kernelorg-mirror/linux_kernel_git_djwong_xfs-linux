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
#include "xfs_health.h"
#include "xfs_healthmon.h"
#include "xfs_fsops.h"

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
 * Please see the xfs_healthmon.schema.json file for a description of the
 * format of the json events that are conveyed to userspace.
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

	/* live update hooks */
	struct xfs_shutdown_hook	shook;
	struct xfs_health_hook		hhook;

	/* filesystem mount, or NULL if we've unmounted */
	struct xfs_mount		*mp;

	/* filesystem type for safe cleanup of hooks; requires module_get */
	struct file_system_type		*fstyp;

	/* number of events */
	unsigned int			events;

	/*
	 * Buffer for formatting events.  New buffer data are appended to the
	 * end of the seqbuf, and outpos is used to determine where to start
	 * a copy_iter.  Both are protected by inode_lock.
	 */
	struct seq_buf			outbuf;
	size_t				outpos;

	/* do we want all events? */
	bool				verbose;

	/* did we lose an event? */
	bool				lost_prev_event;
};

/* Remove an event from the head of the list. */
static inline void
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
		return;
	}

	if (hm->last_event == head)
		hm->last_event = NULL;
	hm->first_event = head->next;
	hm->events--;
	mutex_unlock(&hm->lock);

	trace_xfs_healthmon_pop(hm->mp, head);
	kfree(event);
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
static inline int
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
	event = xfs_healthmon_alloc(hm, XFS_HEALTHMON_LOST,
			XFS_HEALTHMON_MOUNT);
	if (!event)
		return -ENOMEM;

	hm->lost_prev_event = false;
	xfs_healthmon_push(hm, event);
	return 0;
}

/* Compute the reporting mask. */
static inline bool
xfs_healthmon_event_mask(
	struct xfs_healthmon			*hm,
	enum xfs_health_update_type		type,
	const struct xfs_health_update_params	*hup,
	unsigned int				*mask)
{
	/* Always report unmounts. */
	if (type == XFS_HEALTHUP_UNMOUNT)
		return true;

	/* If we want all events, return all events. */
	if (hm->verbose) {
		*mask = hup->new_mask;
		return true;
	}

	switch (type) {
	case XFS_HEALTHUP_SICK:
		/* Always report runtime corruptions */
		*mask = hup->new_mask;
		break;
	case XFS_HEALTHUP_CORRUPT:
		/* Only report new fsck errors */
		*mask = hup->new_mask & ~hup->old_mask;
		break;
	case XFS_HEALTHUP_HEALTHY:
		/* Only report healthy metadata that got fixed */
		*mask = hup->new_mask & hup->old_mask;
		break;
	case XFS_HEALTHUP_UNMOUNT:
		/* This is here for static enum checking */
		break;
	}

	/* If not in verbose mode, mask state has to change. */
	return *mask != 0;
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
	case XFS_HEALTHUP_RT:
		return XFS_HEALTHMON_RT;
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

/* Add a health event to the reporting queue. */
STATIC int
xfs_healthmon_metadata_hook(
	struct notifier_block		*nb,
	unsigned long			action,
	void				*data)
{
	struct xfs_health_update_params	*hup = data;
	struct xfs_healthmon		*hm;
	struct xfs_healthmon_event	*event;
	enum xfs_health_update_type	type = action;
	unsigned int			mask = 0;
	int				error;

	hm = container_of(nb, struct xfs_healthmon, hhook.health_hook.nb);

	/* Decode event mask and skip events we don't care about. */
	if (!xfs_healthmon_event_mask(hm, type, hup, &mask))
		return NOTIFY_DONE;

	mutex_lock(&hm->lock);

	trace_xfs_healthmon_metadata_hook(hm->mp, action, hup, hm->events,
			hm->lost_prev_event);

	error = xfs_healthmon_start_live_update(hm);
	if (error)
		goto out_unlock;

	if (type == XFS_HEALTHUP_UNMOUNT) {
		/*
		 * The filesystem is unmounting, so we must detach from the
		 * mount.  After this point, the healthmon thread has no
		 * connection to the mounted filesystem.
		 */
		trace_xfs_healthmon_unmount(hm->mp, hm->events,
				hm->lost_prev_event);
		hm->mp = NULL;
		wake_up(&hm->wait);
		goto out_unlock;
	}

	event = xfs_healthmon_alloc(hm, health_update_to_type(type),
			  health_update_to_domain(hup->domain));
	if (!event)
		goto out_unlock;

	/* Ignore the event if it's only reporting a secondary health state. */
	switch (event->domain) {
	case XFS_HEALTHMON_FS:
		event->fsmask = mask & ~XFS_SICK_FS_SECONDARY;
		if (!event->fsmask)
			goto out_event;
		break;
	case XFS_HEALTHMON_RT:
		event->fsmask = mask & ~XFS_SICK_RT_SECONDARY;
		if (!event->fsmask)
			goto out_event;
		break;
	case XFS_HEALTHMON_AG:
		event->grpmask = mask & ~XFS_SICK_AG_SECONDARY;
		if (!event->grpmask)
			goto out_event;
		event->group = hup->group;
		break;
	case XFS_HEALTHMON_RTGROUP:
		event->grpmask = mask & ~XFS_SICK_RG_SECONDARY;
		if (!event->grpmask)
			goto out_event;
		event->group = hup->group;
		break;
	case XFS_HEALTHMON_INODE:
		event->imask = mask & ~XFS_SICK_INO_SECONDARY;
		if (!event->imask)
			goto out_event;
		event->ino = hup->ino;
		event->gen = hup->gen;
		break;
	default:
		ASSERT(0);
		break;
	}
	error = xfs_healthmon_push(hm, event);
	if (error)
		goto out_event;

out_unlock:
	mutex_unlock(&hm->lock);
	return NOTIFY_DONE;
out_event:
	kfree(event);
	goto out_unlock;
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

	event = xfs_healthmon_alloc(hm, XFS_HEALTHMON_SHUTDOWN,
			XFS_HEALTHMON_MOUNT);
	if (!event)
		goto out_unlock;

	event->flags = action;
	error = xfs_healthmon_push(hm, event);
	if (error)
		kfree(event);

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
		[XFS_HEALTHMON_UNMOUNT]		= "unmount",
		[XFS_HEALTHMON_SICK]		= "sick",
		[XFS_HEALTHMON_CORRUPT]		= "corrupt",
		[XFS_HEALTHMON_HEALTHY]		= "healthy",
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
		[XFS_HEALTHMON_FS]		= "fs",
		[XFS_HEALTHMON_RT]		= "realtime",
		[XFS_HEALTHMON_AG]		= "perag",
		[XFS_HEALTHMON_INODE]		= "inode",
		[XFS_HEALTHMON_RTGROUP]		= "rtgroup",
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

		if (!p->str) {
			flags &= ~p->mask;
			continue;
		}

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

/* Render fs sickness mask as a string set */
static int
xfs_healthmon_format_fs(
	struct seq_buf			*outbuf,
	const struct xfs_healthmon_event *event)
{
	static const struct flag_string	mask_strings[] = {
		{ XFS_SICK_FS_COUNTERS,		"fscounters" },
		{ XFS_SICK_FS_UQUOTA,		"usrquota" },
		{ XFS_SICK_FS_GQUOTA,		"grpquota" },
		{ XFS_SICK_FS_PQUOTA,		"prjquota" },
		{ XFS_SICK_FS_QUOTACHECK,	"quotacheck" },
		{ XFS_SICK_FS_NLINKS,		"nlinks" },
		{ XFS_SICK_FS_METADIR,		"metadir" },
		{ XFS_SICK_FS_METAPATH,		"metapath" },
	};

	return xfs_healthmon_format_mask(outbuf, "structures", mask_strings,
			event->fsmask);
}

/* Render rt sickness mask as a string set */
static int
xfs_healthmon_format_rt(
	struct seq_buf			*outbuf,
	const struct xfs_healthmon_event *event)
{
	static const struct flag_string	mask_strings[] = {
		{ XFS_SICK_RT_BITMAP,		"bitmap" },
		{ XFS_SICK_RT_SUMMARY,		"summary" },
	};

	return xfs_healthmon_format_mask(outbuf, "structures", mask_strings,
			event->fsmask);
}

/* Render rtgroup sickness mask as a string set */
static int
xfs_healthmon_format_rtgroup(
	struct seq_buf			*outbuf,
	const struct xfs_healthmon_event *event)
{
	static const struct flag_string	mask_strings[] = {
		{ XFS_SICK_RG_SUPER,		"super" },
		{ XFS_SICK_RG_BITMAP,		"bitmap" },
		{ XFS_SICK_RG_RMAPBT,		"rmapbt" },
		{ XFS_SICK_RG_REFCNTBT,		"refcountbt" },
	};
	ssize_t				ret;

	ret = xfs_healthmon_format_mask(outbuf, "structures", mask_strings,
			event->grpmask);
	if (ret < 0)
		return ret;

	return seq_buf_printf(outbuf, "  \"group\":      %u,\n",
			event->group);
}

/* Render perag sickness mask as a string set */
static int
xfs_healthmon_format_ag(
	struct seq_buf			*outbuf,
	const struct xfs_healthmon_event *event)
{
	static const struct flag_string	mask_strings[] = {
		{ XFS_SICK_AG_SB,		"super" },
		{ XFS_SICK_AG_AGF,		"agf" },
		{ XFS_SICK_AG_AGFL,		"agfl" },
		{ XFS_SICK_AG_AGI,		"agi" },
		{ XFS_SICK_AG_BNOBT,		"bnobt" },
		{ XFS_SICK_AG_CNTBT,		"cntbt" },
		{ XFS_SICK_AG_INOBT,		"inobt" },
		{ XFS_SICK_AG_FINOBT,		"finobt" },
		{ XFS_SICK_AG_RMAPBT,		"rmapbt" },
		{ XFS_SICK_AG_REFCNTBT,		"refcountbt" },
		{ XFS_SICK_AG_INODES,		"inodes" },
	};
	ssize_t				ret;

	ret = xfs_healthmon_format_mask(outbuf, "structures", mask_strings,
			event->grpmask);
	if (ret < 0)
		return ret;

	return seq_buf_printf(outbuf, "  \"group\":      %u,\n",
			event->group);
}

/* Render inode sickness mask as a string set */
static int
xfs_healthmon_format_inode(
	struct seq_buf			*outbuf,
	const struct xfs_healthmon_event *event)
{
	static const struct flag_string	mask_strings[] = {
		{ XFS_SICK_INO_CORE,		"core" },
		{ XFS_SICK_INO_BMBTD,		"bmapbtd" },
		{ XFS_SICK_INO_BMBTA,		"bmapbta" },
		{ XFS_SICK_INO_BMBTC,		"bmapbtc" },
		{ XFS_SICK_INO_DIR,		"directory" },
		{ XFS_SICK_INO_XATTR,		"xattr" },
		{ XFS_SICK_INO_SYMLINK,		"symlink" },
		{ XFS_SICK_INO_PARENT,		"parent" },
		{ XFS_SICK_INO_BMBTD_ZAPPED,	"bmapbtd_zapped" },
		{ XFS_SICK_INO_BMBTA_ZAPPED,	"bmapbta_zapped" },
		{ XFS_SICK_INO_DIR_ZAPPED,	"directory_zapped" },
		{ XFS_SICK_INO_SYMLINK_ZAPPED,	"symlink_zapped" },
		{ XFS_SICK_INO_FORGET,		NULL, },
		{ XFS_SICK_INO_DIRTREE,		"dirtree" },
	};
	ssize_t				ret;

	ret = xfs_healthmon_format_mask(outbuf, "structures", mask_strings,
			event->imask);
	if (ret < 0)
		return ret;

	ret = seq_buf_printf(outbuf, "  \"inumber\":    %llu,\n",
			event->ino);
	if (ret < 0)
		return ret;
	return seq_buf_printf(outbuf, "  \"generation\": %u,\n",
			event->gen);
}

/* Render shutdown mask as a string set */
static int
xfs_healthmon_format_shutdown(
	struct seq_buf			*outbuf,
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

	return xfs_healthmon_format_mask(outbuf, "reasons", mask_strings,
			event->flags);
}

static inline void
xfs_healthmon_reset_outbuf(
	struct xfs_healthmon		*hm)
{
	hm->outpos = 0;
	seq_buf_clear(&hm->outbuf);
}

/*
 * Format an event into json.  Returns 0 if we formatted the event.  If
 * formatting the event overflows the buffer, returns -1 with the seqbuf len
 * unchanged.
 */
STATIC int
xfs_healthmon_format(
	struct xfs_healthmon		*hm,
	const struct xfs_healthmon_event *event)
{
	struct seq_buf			*outbuf = &hm->outbuf;
	size_t				old_seqlen = outbuf->len;
	int				ret;

	trace_xfs_healthmon_format(hm->mp, event);

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
	case XFS_HEALTHMON_SHUTDOWN:
		ret = xfs_healthmon_format_shutdown(outbuf, event);
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
	case XFS_HEALTHMON_FS:
		ret = xfs_healthmon_format_fs(outbuf, event);
		break;
	case XFS_HEALTHMON_RT:
		ret = xfs_healthmon_format_rt(outbuf, event);
		break;
	case XFS_HEALTHMON_RTGROUP:
		ret = xfs_healthmon_format_rtgroup(outbuf, event);
		break;
	case XFS_HEALTHMON_AG:
		ret = xfs_healthmon_format_ag(outbuf, event);
		break;
	case XFS_HEALTHMON_INODE:
		ret = xfs_healthmon_format_inode(outbuf, event);
		break;
	}
	if (ret < 0)
		goto overrun;

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
	 * We overflowed the buffer and could not format the event.  Reset the
	 * seqbuf and tell the caller not to delete the event.
	 */
	trace_xfs_healthmon_format_overflow(hm->mp, event);
	outbuf->len = old_seqlen;
	return -1;
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

	trace_xfs_healthmon_copybuf(hm->mp, to, &hm->outbuf, hm->outpos);

	to_copy = xfs_healthmon_outbuf_bytes(hm);
	if (to_copy) {
		w = copy_to_iter(hm->outbuf.buffer + hm->outpos, to_copy, to);
		if (!w)
			return -EFAULT;

		hm->outpos += w;
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
	if (hm->outbuf.size == 0) {
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
		if (hm->outbuf.size == 0) {
			seq_buf_init(&hm->outbuf, outbuf, bufsize);
			hm->outpos = 0;
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
			ret = xfs_healthmon_format(hm, event);
			if (ret < 0)
				break;
			xfs_healthmon_free_head(hm, event);
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
	xfs_shutdown_hook_disable();
	xfs_health_hook_disable();

	mutex_destroy(&hm->lock);
	xfs_healthmon_free_events(hm);
	if (hm->outbuf.size)
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

	/*
	 * Since we already got a ref to the module, take a reference to the
	 * fstype to make it easier to detach the hooks when we tear things
	 * down later.
	 */
	hm->fstyp = mp->m_super->s_type;

	seq_buf_init(&hm->outbuf, NULL, 0);
	mutex_init(&hm->lock);
	init_waitqueue_head(&hm->wait);

	if (hmo.flags & XFS_HEALTH_MONITOR_VERBOSE)
		hm->verbose = true;

	/* Enable hooks to receive events, generally. */
	xfs_health_hook_enable();
	xfs_shutdown_hook_enable();

	/* Attach specific event hooks to this monitor. */
	xfs_health_hook_setup(&hm->hhook, xfs_healthmon_metadata_hook);
	ret = xfs_health_hook_add(mp, &hm->hhook);
	if (ret)
		goto out_hooks;

	xfs_shutdown_hook_setup(&hm->shook, xfs_healthmon_shutdown_hook);
	ret = xfs_shutdown_hook_add(mp, &hm->shook);
	if (ret)
		goto out_healthhook;

	/* Set up VFS file and file descriptor. */
	ret = get_unused_fd_flags(fd_flags);
	if (ret < 0)
		goto out_shutdownhook;
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
out_shutdownhook:
	xfs_shutdown_hook_del(mp, &hm->shook);
out_healthhook:
	xfs_health_hook_del(mp, &hm->hhook);
out_hooks:
	xfs_health_hook_disable();
	xfs_shutdown_hook_disable();
	mutex_destroy(&hm->lock);
	xfs_healthmon_free_events(hm);
	kfree(hm);
out_mod:
	module_put(THIS_MODULE);
	return ret;
}
