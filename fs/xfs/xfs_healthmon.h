/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_HEALTHMON_H__
#define __XFS_HEALTHMON_H__

enum xfs_healthmon_type {
	XFS_HEALTHMON_LOST,	/* message lost */

	/* filesystem shutdown */
	XFS_HEALTHMON_SHUTDOWN,
};

enum xfs_healthmon_domain {
	XFS_HEALTHMON_MOUNT,	/* affects the whole fs */
};

struct xfs_healthmon_event {
	struct xfs_healthmon_event	*next;

	enum xfs_healthmon_type		type;
	enum xfs_healthmon_domain	domain;

	uint64_t			time_ns;

	union {
		/* mount */
		struct {
			unsigned int	flags;
		};
	};
};

#ifdef CONFIG_XFS_HEALTH_MONITOR
int xfs_healthmon_create(struct xfs_mount *mp, struct xfs_health_monitor *hmo);
#else
# define xfs_healthmon_create(mp, hmo)		(-EOPNOTSUPP)
#endif /* CONFIG_XFS_HEALTH_MONITOR */

#endif /* __XFS_HEALTHMON_H__ */
