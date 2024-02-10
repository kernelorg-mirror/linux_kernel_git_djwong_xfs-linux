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
#include "xfs_health.h"
#include "xfs_healthmon.h"
#include "xfs_notify_failure.h"
#include "xfs_fs.h"
#include "xfs_ioctl.h"

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
 *
 * Metadata Health Events
 * ----------------------
 *
 * {
 *	"type": "sick" | "corrupt" | "healthy",
 *	"domain": "fs" | "realtime" | "ag" | "inode" | "rtgroup",
 *	"structures": [structure string list...],
 *
 *	"group": integer,      (if domain is "ag" or "rtgroup")
 *
 *	"inode": integer,      (if domain is "inode")
 *	"generation": integer, (if domain is "inode")
 *
 *	"time_ns": integer
 * }
 *
 * "sick" means that metadata corruption was discovered during a runtime
 * operation.
 *
 * "corrupt" means that corruption was discovered during an xfs_scrub run.
 *
 * "healthy" means that a metadata object was found to be ok by xfs_scrub.
 *
 * The domain item indicates where in the filesystem to find the metadata
 * object(s) that are the target of the event.
 *
 * "fs" means whole-filesystem metadata.  Structures are as follows:
 *
 *     "fscounters": summary counters
 *     "usrquota":   user quota records
 *     "grpquota":   group quota records
 *     "prjquota":   project quota records
 *     "quotacheck": quota counters
 *     "nlinks":     file link counts
 *     "metadir":    metadata directory
 *     "metapath":   metadata inode paths
 *
 * "realtime" means realtime volume metadata:
 *
 *     "bitmap":     realtime bitmap file
 *     "summary":    realtime free space summary file
 *
 * "ag" means allocation group metadata on the data device:
 *
 *     "super":      superblock
 *     "agf":        group space header
 *     "agfl":       per-group free block list
 *     "agi":        group inode header
 *     "bnobt":      free space by position btree
 *     "cntbt":      free space by length btree
 *     "inobt":      inode btree
 *     "finobt":     free inode btree
 *     "rmapbt":     reverse mapping btree
 *     "refcountbt": reference count btree
 *     "inodes":     problems were recorded for this group's inodes, but the
 *                   inodes themselves had to be reclaimed
 *
 * "inode" means inode metadata:
 *
 *     "core":       inode record
 *     "bmapbtd":    data fork
 *     "bmapbta":    attr fork
 *     "bmapbtc":    cow fork
 *     "directory":  directory entries and index
 *     "xattr":      extended attributes and index
 *     "symlink":    symbolic link target
 *     "parent":     directory parent pointer
 *     "bmapbtd_zapped":  these are set when an inode record repair had to drop
 *     "bmapbtd_zapped"   the corresponding data structure to get the inode
 *     "directory_zapped" back to a consistent state
 *     "symlink_zapped"
 *     "dirtree":    directory tree problems detected
 *
 * "rtgroup" means realtime group metadata for the realtime volume:
 *
 *     "super":      group superblock
 *     "bitmap":     free space bitmap contents for this group
 *     "rmapbt":     reverse mapping btree
 *     "refcountbt": reference count btree
 *
 * Media Failures
 * --------------
 *
 * {
 *	"type": "media",
 *	"domain": "datadev" | "logdev" | "rtdev",
 *	"daddr": integer,
 *	"bbcount": integer,
 *	"time_ns": integer
 * }
 *
 * The domain element tells us which device reported a media failure.  The
 * daddr and bbcount elements tell us where inside that device the failure was
 * observed.
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
	struct xfs_health_hook		hhook;
	struct xfs_media_error_hook	mhook;

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
		xfs_media_error_hook_del(hm->mp, &hm->mhook);
		xfs_health_hook_del(hm->mp, &hm->hhook);
		xfs_shutdown_hook_del(hm->mp, &hm->shook);
	}
	xfs_media_error_hook_disable();
	xfs_health_hook_disable();
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

	event = new_event(hm, health_update_to_type(type),
			  health_update_to_domain(hup->domain));
	if (!event)
		goto out_unlock;

	switch (event->domain) {
	case XFS_HEALTHMON_FS:
	case XFS_HEALTHMON_RT:
		event->fsmask = mask;
		break;
	case XFS_HEALTHMON_AG:
	case XFS_HEALTHMON_RTGROUP:
		event->grpmask = mask;
		event->group = hup->group;
		break;
	case XFS_HEALTHMON_INODE:
		event->imask = mask;
		event->ino = hup->ino;
		event->gen = hup->gen;
		break;
	default:
		ASSERT(0);
		break;
	}
	xfs_healthmon_push(hm, event);

out_unlock:
	mutex_unlock(&hm->lock);
	return NOTIFY_DONE;
}

#if defined(CONFIG_MEMORY_FAILURE) && defined(CONFIG_FS_DAX)
/* Add a media error event to the reporting queue. */
STATIC int
xfs_healthmon_media_error_hook(
	struct notifier_block		*nb,
	unsigned long			action,
	void				*data)
{
	struct xfs_healthmon		*hm;
	struct xfs_healthmon_event	*event;
	struct xfs_media_error_params	*p = data;
	struct xfs_mount		*mp = p->btp->bt_mount;
	enum xfs_healthmon_domain	domain;
	int				error;

	hm = container_of(nb, struct xfs_healthmon, mhook.error_hook.nb);

	mutex_lock(&hm->lock);

	trace_xfs_healthmon_media_error_hook(hm->mp, p, hm->events,
			hm->lost_prev_event);

	error = xfs_healthmon_start_live_update(hm);
	if (error)
		goto out_unlock;

	if (mp->m_logdev_targp != mp->m_ddev_targp &&
	    mp->m_logdev_targp == p->btp) {
		domain = XFS_HEALTHMON_LOGDEV;
	} else if (mp->m_rtdev_targp == p->btp) {
		domain = XFS_HEALTHMON_RTDEV;
	} else {
		domain = XFS_HEALTHMON_DATADEV;
	}

	event = new_event(hm, XFS_HEALTHMON_MEDIA_ERROR, domain);
	if (!event)
		goto out_unlock;

	event->daddr = p->daddr;
	event->bbcount = p->bbcount;
	xfs_healthmon_push(hm, event);

out_unlock:
	mutex_unlock(&hm->lock);
	return NOTIFY_DONE;
}
#endif

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
		[XFS_HEALTHMON_MEDIA_ERROR]	= "media",
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
		[XFS_HEALTHMON_AG]		= "ag",
		[XFS_HEALTHMON_INODE]		= "inode",
		[XFS_HEALTHMON_RTGROUP]		= "rtgroup",
		[XFS_HEALTHMON_DATADEV]		= "datadev",
		[XFS_HEALTHMON_LOGDEV]		= "logdev",
		[XFS_HEALTHMON_RTDEV]		= "rtdev",
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

		if (!p->str) {
			flags &= ~p->mask;
			continue;
		}

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

/* Render fs sickness mask as a string set */
static ssize_t
xfs_healthmon_format_fs(
	struct stdio_redirect		*out,
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

	return xfs_healthmon_format_mask(out, "structures", mask_strings,
			event->fsmask);
}

/* Render rt sickness mask as a string set */
static ssize_t
xfs_healthmon_format_rt(
	struct stdio_redirect		*out,
	const struct xfs_healthmon_event *event)
{
	static const struct flag_string	mask_strings[] = {
		{ XFS_SICK_RT_BITMAP,		"bitmap" },
		{ XFS_SICK_RT_SUMMARY,		"summary" },
	};

	return xfs_healthmon_format_mask(out, "structures", mask_strings,
			event->fsmask);
}

/* Render rtgroup sickness mask as a string set */
static ssize_t
xfs_healthmon_format_rtgroup(
	struct stdio_redirect		*out,
	const struct xfs_healthmon_event *event)
{
	static const struct flag_string	mask_strings[] = {
		{ XFS_SICK_RG_SUPER,		"super" },
		{ XFS_SICK_RG_BITMAP,		"bitmap" },
		{ XFS_SICK_RG_RMAPBT,		"rmapbt" },
		{ XFS_SICK_RG_REFCNTBT,		"refcountbt" },
	};
	ssize_t				ret;

	ret = xfs_healthmon_format_mask(out, "structures", mask_strings,
			event->grpmask);
	if (ret < 0)
		return ret;

	return stdio_redirect_printf(out, false, "  \"group\":      %u,\n",
			event->group);
}

/* Render perag sickness mask as a string set */
static ssize_t
xfs_healthmon_format_ag(
	struct stdio_redirect		*out,
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

	ret = xfs_healthmon_format_mask(out, "structures", mask_strings,
			event->grpmask);
	if (ret < 0)
		return ret;

	return stdio_redirect_printf(out, false, "  \"group\":      %u,\n",
			event->group);
}

/* Render inode sickness mask as a string set */
static ssize_t
xfs_healthmon_format_inode(
	struct stdio_redirect		*out,
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
		{ XFS_SICK_INO_BMBTA_ZAPPED,	"bmapbtd_zapped" },
		{ XFS_SICK_INO_DIR_ZAPPED,	"directory_zapped" },
		{ XFS_SICK_INO_SYMLINK_ZAPPED,	"symlink_zapped" },
		{ XFS_SICK_INO_FORGET,		NULL, },
		{ XFS_SICK_INO_DIRTREE,		"dirtree" },
	};
	ssize_t				ret;

	ret = xfs_healthmon_format_mask(out, "structures", mask_strings,
			event->imask);
	if (ret < 0)
		return ret;

	ret = stdio_redirect_printf(out, false, "  \"inode\":      %llu,\n",
			event->ino);
	if (ret < 0)
		return ret;
	return stdio_redirect_printf(out, false, "  \"generation\": %u,\n",
			event->gen);
}

/* Render media error as a string set */
static ssize_t
xfs_healthmon_format_media_error(
	struct stdio_redirect		*out,
	const struct xfs_healthmon_event *event)
{
	ssize_t				ret;

	ret = stdio_redirect_printf(out, false, "  \"daddr\":      %llu,\n",
			event->daddr);
	if (ret < 0)
		return ret;

	return stdio_redirect_printf(out, false, "  \"bbcount\":    %llu,\n",
			event->bbcount);
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
	case XFS_HEALTHMON_FS:
		ret = xfs_healthmon_format_fs(out, event);
		break;
	case XFS_HEALTHMON_RT:
		ret = xfs_healthmon_format_rt(out, event);
		break;
	case XFS_HEALTHMON_RTGROUP:
		ret = xfs_healthmon_format_rtgroup(out, event);
		break;
	case XFS_HEALTHMON_AG:
		ret = xfs_healthmon_format_ag(out, event);
		break;
	case XFS_HEALTHMON_INODE:
		ret = xfs_healthmon_format_inode(out, event);
		break;
	case XFS_HEALTHMON_DATADEV:
	case XFS_HEALTHMON_LOGDEV:
	case XFS_HEALTHMON_RTDEV:
		ret = xfs_healthmon_format_media_error(out, event);
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

/* Pipe health monitoring information to userspace. */
STATIC void
xfs_healthmon_run(
	struct thread_with_stdio	*thr)
{
	struct xfs_healthmon		*hm = to_healthmon(thr);
	struct xfs_healthmon_event	*event;
	bool				unmounted = false;

	while (!kthread_should_stop() && !unmounted &&
	       wait_event_interruptible(hm->wait,
				hm->events > 0 || hm->mp == NULL) == 0) {

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

/* Handle ioctls for the health monitoring thread. */
STATIC long
xfs_healthmon_ioctl(
	struct thread_with_stdio	*thr,
	unsigned int			cmd,
	unsigned long			p)
{
	struct xfs_health_monitor	hmo;
	struct xfs_healthmon		*hm = to_healthmon(thr);
	void			__user *arg = (void __user *)p;

	if (cmd != XFS_IOC_HEALTH_MONITOR)
		return -ENOTTY;

	if (copy_from_user(&hmo, arg, sizeof(hmo)))
		return -EFAULT;

	if (!xfs_healthmon_validate(&hmo))
		return -EINVAL;

	mutex_lock(&hm->lock);
	hm->verbose = !!(hmo.flags & XFS_HEALTH_MONITOR_VERBOSE);
	mutex_unlock(&hm->lock);
	return 0;
}

static const struct thread_with_stdio_ops xfs_healthmon_ops = {
	.exit		= xfs_healthmon_exit,
	.fn		= xfs_healthmon_run,
	.unlocked_ioctl	= xfs_healthmon_ioctl,
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

	hm->mp = mp;
	mutex_init(&hm->lock);
	init_waitqueue_head(&hm->wait);

	if (hmo->flags & XFS_HEALTH_MONITOR_VERBOSE)
		hm->verbose = true;

	xfs_shutdown_hook_enable();
	xfs_health_hook_enable();
	xfs_media_error_hook_enable();

	xfs_shutdown_hook_setup(&hm->shook, xfs_healthmon_shutdown_hook);
	ret = xfs_shutdown_hook_add(mp, &hm->shook);
	if (ret)
		goto out_hooks;

	xfs_health_hook_setup(&hm->hhook, xfs_healthmon_metadata_hook);
	ret = xfs_health_hook_add(mp, &hm->hhook);
	if (ret)
		goto out_shutdown;

	xfs_media_error_hook_setup(&hm->mhook, xfs_healthmon_media_error_hook);
	ret = xfs_media_error_hook_add(mp, &hm->mhook);
	if (ret)
		goto out_health;

	ret = run_thread_with_stdout(&hm->thread, &xfs_healthmon_ops);
	if (ret < 0)
		goto out_media;

	trace_xfs_healthmon_create(mp, hmo->flags, hmo->format);

	return ret;
out_media:
	xfs_media_error_hook_del(mp, &hm->mhook);
out_health:
	xfs_health_hook_del(mp, &hm->hhook);
out_shutdown:
	xfs_shutdown_hook_del(mp, &hm->shook);
out_hooks:
	xfs_media_error_hook_disable();
	xfs_health_hook_disable();
	xfs_shutdown_hook_disable();
	mutex_destroy(&hm->lock);
	xfs_healthmon_free_events(hm);
	kfree(hm);
out_mod:
	module_put(THIS_MODULE);
	return ret;
}
