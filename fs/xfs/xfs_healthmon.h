/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_HEALTHMON_H__
#define __XFS_HEALTHMON_H__

#ifdef CONFIG_XFS_HEALTH_MONITOR
int xfs_healthmon_create(struct xfs_mount *mp, struct xfs_health_monitor *hmo);
#else
# define xfs_healthmon_create(mp, hmo)		(-EOPNOTSUPP)
#endif /* CONFIG_XFS_HEALTH_MONITOR */

#endif /* __XFS_HEALTHMON_H__ */
