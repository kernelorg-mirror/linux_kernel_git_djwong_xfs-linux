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
};

struct xfs_healthmon *xfs_healthmon_get(struct xfs_mount *mp);
void xfs_healthmon_put(struct xfs_healthmon *hm);

#define with_xfs_healthmon(mp, data) \
	for (struct xfs_healthmon *data = xfs_healthmon_get(mp); \
	     data != NULL; \
	     xfs_healthmon_put(data), data = NULL)

void xfs_healthmon_detach(struct xfs_mount *mp);

long xfs_ioc_health_monitor(struct file *file,
		struct xfs_health_monitor __user *arg);

#endif /* __XFS_HEALTHMON_H__ */
