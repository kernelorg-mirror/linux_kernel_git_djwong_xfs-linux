/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2024-2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_HEALTHMON_H__
#define __XFS_HEALTHMON_H__

struct xfs_healthmon {
	/* Filesystem type for use with iter_supers_type. */
	struct file_system_type		*fstyp;

	/*
	 * Weak reference to the xfs_mount that might point to this health
	 * monitor.  If you want to dereference this pointer you must hold
	 * s_umount or something else that prevents unmount.
	 */
	struct xfs_mount		*mp;

	/*
	 * Device number of the filesystem being monitored.  This is for
	 * consistent tracing even after unmount.
	 */
	dev_t				dev;

	/*
	 * Refcount to this structure.  The open healthmon fd holds one ref,
	 * the xfs_mount holds another ref if it points to this object, and
	 * running event handlers hold their own refs.
	 */
	atomic_t			ref;

	/* lock for event list and event counters */
	struct mutex			lock;

	/* list of event objects */
	struct xfs_healthmon_event	*first_event;
	struct xfs_healthmon_event	*last_event;

	/* number of events in the list */
	unsigned int			events;

	/* waiter so read/poll can sleep until the arrival of events */
	struct wait_queue_head		wait;

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

struct xfs_healthmon *xfs_healthmon_get(struct xfs_mount *mp);
void xfs_healthmon_put(struct xfs_healthmon *hm);

#define with_xfs_healthmon(mp, data) \
	for (struct xfs_healthmon *data = xfs_healthmon_get(mp); \
	     data != NULL; \
	     xfs_healthmon_put(data), data = NULL)

void xfs_healthmon_detach(struct xfs_mount *mp);

enum xfs_healthmon_type {
	XFS_HEALTHMON_RUNNING,	/* monitor running */
	XFS_HEALTHMON_LOST,	/* message lost */
	XFS_HEALTHMON_UNMOUNT,	/* filesystem is unmounting */

	/* filesystem shutdown */
	XFS_HEALTHMON_SHUTDOWN,

	/* metadata health events */
	XFS_HEALTHMON_SICK,	/* runtime corruption observed */
	XFS_HEALTHMON_CORRUPT,	/* fsck reported corruption */
	XFS_HEALTHMON_HEALTHY,	/* fsck reported healthy structure */

	/* media errors */
	XFS_HEALTHMON_MEDIA_ERROR,
};

enum xfs_healthmon_domain {
	XFS_HEALTHMON_MOUNT,	/* affects the whole fs */

	/* metadata health events */
	XFS_HEALTHMON_FS,	/* main filesystem metadata */
	XFS_HEALTHMON_AG,	/* allocation group metadata */
	XFS_HEALTHMON_INODE,	/* inode metadata */
	XFS_HEALTHMON_RTGROUP,	/* realtime group metadata */

	/* media errors */
	XFS_HEALTHMON_DATADEV,
	XFS_HEALTHMON_RTDEV,
	XFS_HEALTHMON_LOGDEV,
};

struct xfs_healthmon_event {
	struct xfs_healthmon_event	*next;

	enum xfs_healthmon_type		type;
	enum xfs_healthmon_domain	domain;

	uint64_t			time_ns;

	union {
		/* lost events */
		struct {
			uint64_t	lostcount;
		};
		/* shutdown */
		struct {
			unsigned int	flags;
		};
		/* fs/rt metadata */
		struct {
			/* XFS_SICK_* flags */
			unsigned int	fsmask;
		};
		/* ag/rtgroup metadata */
		struct {
			/* XFS_SICK_* flags */
			unsigned int	grpmask;
			unsigned int	group;
		};
		/* inode metadata */
		struct {
			/* XFS_SICK_INO_* flags */
			unsigned int	imask;
			uint32_t	gen;
			xfs_ino_t	ino;
		};
		/* media errors */
		struct {
			xfs_daddr_t	daddr;
			uint64_t	bbcount;
		};
	};
};

struct xfs_health_update_params;
void xfs_healthmon_metadata_hook(struct xfs_healthmon *hmon,
		const struct xfs_health_update_params *p);

void xfs_healthmon_shutdown_hook(struct xfs_healthmon *hmon, uint32_t flags);

struct xfs_media_error_params;
void xfs_healthmon_media_error_hook(struct xfs_healthmon *hmon,
		const struct xfs_media_error_params *p);

long xfs_ioc_health_monitor(struct file *file,
		struct xfs_health_monitor __user *arg);

#endif /* __XFS_HEALTHMON_H__ */
