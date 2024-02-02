// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_TIMESTATS_H__
#define __XFS_TIMESTATS_H__

#ifdef CONFIG_XFS_TIME_STATS
extern const struct file_operations xfs_timestats_fops;

void xfs_timestats_init(struct xfs_mount *mp);
void xfs_timestats_export(struct xfs_mount *mp);
void xfs_timestats_unexport(struct xfs_mount *mp);
void xfs_timestats_destroy(struct xfs_mount *mp);

# define DECLARE_XFS_TIMESTAT(name)	u64 name
# define DEFINE_XFS_TIMESTAT(name)	u64 name = local_clock()
# define xfs_timestats_start(b)		do { *(b) = local_clock(); } while (0)
# define xfs_timestats_end(a, b)	time_stats_update((a), (b))
# define xfs_timestats_interval(a,b,c)	__time_stats_update((a), (b), (c))
#else
# define xfs_timestats_init(mp)		((void)0)
# define xfs_timestats_export(mp)	((void)0)
# define xfs_timestats_unexport(mp)	((void)0)
# define xfs_timestats_destroy(mp)	((void)0)

# define DECLARE_XFS_TIMESTAT(name)
# define DEFINE_XFS_TIMESTAT(name)
# define xfs_timestats_start(t)		((void)0)
# define xfs_timestats_end(s, t)	((void)0)
# define xfs_timestats_interval(...)	((void)0)
#endif /* CONFIG_XFS_TIME_STATS */

#endif /* __XFS_TIMESTATS_H__ */

