// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_HEALTH_H__
#define __XFS_HEALTH_H__

/*
 * In-Core Filesystem Health Assessments
 * =====================================
 *
 * We'd like to be able to summarize the current health status of the
 * filesystem so that the administrator knows when it's necessary to schedule
 * some downtime for repairs.  Until then, we would also like to avoid abrupt
 * shutdowns due to corrupt metadata.
 *
 * The online scrub feature evaluates the health of all filesystem metadata.
 * When scrub detects corruption in a piece of metadata it will set the
 * corresponding sickness flag, and repair will clear it if successful.
 *
 * If problems remain at unmount time, we can also request manual intervention
 * by logging a notice to run xfs_repair.
 *
 * Evidence of health problems can be sorted into three basic categories:
 *
 * a) Primary evidence, which signals that something is defective within the
 *    general grouping of metadata.
 *
 * b) Secondary evidence, which are side effects of primary problem but are
 *    not themselves problems.  These can be forgotten when the primary
 *    health problems are addressed.
 *
 * c) Indirect evidence, which points to something being wrong in another
 *    group, but we had to release resources and this is all that's left of
 *    that state.
 */

struct xfs_mount;
struct xfs_perag;
struct xfs_inode;

/* Observable health issues for metadata spanning the entire filesystem. */
#define XFS_HEALTH_FS_COUNTERS	(1 << 0)  /* summary counters */
#define XFS_HEALTH_FS_UQUOTA	(1 << 1)  /* user quota */
#define XFS_HEALTH_FS_GQUOTA	(1 << 2)  /* group quota */
#define XFS_HEALTH_FS_PQUOTA	(1 << 3)  /* project quota */

/* Observable health issues for realtime volume metadata. */
#define XFS_HEALTH_RT_BITMAP	(1 << 0)  /* realtime bitmap */
#define XFS_HEALTH_RT_SUMMARY	(1 << 1)  /* realtime summary */

/* Observable health issues for AG metadata. */
#define XFS_HEALTH_AG_SB	(1 << 0)  /* superblock */
#define XFS_HEALTH_AG_AGF	(1 << 1)  /* AGF header */
#define XFS_HEALTH_AG_AGFL	(1 << 2)  /* AGFL header */
#define XFS_HEALTH_AG_AGI	(1 << 3)  /* AGI header */
#define XFS_HEALTH_AG_BNOBT	(1 << 4)  /* free space by block */
#define XFS_HEALTH_AG_CNTBT	(1 << 5)  /* free space by length */
#define XFS_HEALTH_AG_INOBT	(1 << 6)  /* inode index */
#define XFS_HEALTH_AG_FINOBT	(1 << 7)  /* free inode index */
#define XFS_HEALTH_AG_RMAPBT	(1 << 8)  /* reverse mappings */
#define XFS_HEALTH_AG_REFCNTBT	(1 << 9)  /* reference counts */

/* Observable health issues for inode metadata. */
#define XFS_HEALTH_INO_CORE	(1 << 0)  /* inode core */
#define XFS_HEALTH_INO_BMBTD	(1 << 1)  /* data fork */
#define XFS_HEALTH_INO_BMBTA	(1 << 2)  /* attr fork */
#define XFS_HEALTH_INO_BMBTC	(1 << 3)  /* cow fork */
#define XFS_HEALTH_INO_DIR	(1 << 4)  /* directory */
#define XFS_HEALTH_INO_XATTR	(1 << 5)  /* extended attributes */
#define XFS_HEALTH_INO_SYMLINK	(1 << 6)  /* symbolic link remote target */
#define XFS_HEALTH_INO_PARENT	(1 << 7)  /* parent pointers */

/* Primary evidence of health problems in a given group. */
#define XFS_HEALTH_FS_PRIMARY	(XFS_HEALTH_FS_COUNTERS | \
				 XFS_HEALTH_FS_UQUOTA | \
				 XFS_HEALTH_FS_GQUOTA | \
				 XFS_HEALTH_FS_PQUOTA)

#define XFS_HEALTH_RT_PRIMARY	(XFS_HEALTH_RT_BITMAP | \
				 XFS_HEALTH_RT_SUMMARY)

#define XFS_HEALTH_AG_PRIMARY	(XFS_HEALTH_AG_SB | \
				 XFS_HEALTH_AG_AGF | \
				 XFS_HEALTH_AG_AGFL | \
				 XFS_HEALTH_AG_AGI | \
				 XFS_HEALTH_AG_BNOBT | \
				 XFS_HEALTH_AG_CNTBT | \
				 XFS_HEALTH_AG_INOBT | \
				 XFS_HEALTH_AG_FINOBT | \
				 XFS_HEALTH_AG_RMAPBT | \
				 XFS_HEALTH_AG_REFCNTBT)

#define XFS_HEALTH_INO_PRIMARY	(XFS_HEALTH_INO_CORE | \
				 XFS_HEALTH_INO_BMBTD | \
				 XFS_HEALTH_INO_BMBTA | \
				 XFS_HEALTH_INO_BMBTC | \
				 XFS_HEALTH_INO_DIR | \
				 XFS_HEALTH_INO_XATTR | \
				 XFS_HEALTH_INO_SYMLINK | \
				 XFS_HEALTH_INO_PARENT)

/* Secondary state related to (but not primary evidence of) health problems. */
#define XFS_HEALTH_FS_SECONDARY	(0)
#define XFS_HEALTH_RT_SECONDARY	(0)
#define XFS_HEALTH_AG_SECONDARY	(0)
#define XFS_HEALTH_INO_SECONDARY (0)

/* Evidence of health problems elsewhere. */
#define XFS_HEALTH_FS_INDIRECT	(0)
#define XFS_HEALTH_RT_INDIRECT	(0)
#define XFS_HEALTH_AG_INDIRECT	(0)
#define XFS_HEALTH_INO_INDIRECT	(0)

/* All health masks. */
#define XFS_HEALTH_FS_ALL	(XFS_HEALTH_FS_PRIMARY | \
				 XFS_HEALTH_FS_SECONDARY | \
				 XFS_HEALTH_FS_INDIRECT)

#define XFS_HEALTH_RT_ALL	(XFS_HEALTH_RT_PRIMARY | \
				 XFS_HEALTH_RT_SECONDARY | \
				 XFS_HEALTH_RT_INDIRECT)

#define XFS_HEALTH_AG_ALL	(XFS_HEALTH_AG_PRIMARY | \
				 XFS_HEALTH_AG_SECONDARY | \
				 XFS_HEALTH_AG_INDIRECT)

#define XFS_HEALTH_INO_ALL	(XFS_HEALTH_INO_PRIMARY | \
				 XFS_HEALTH_INO_SECONDARY | \
				 XFS_HEALTH_INO_INDIRECT)

/* These functions must be provided by the xfs implementation. */

void xfs_fs_mark_sick(struct xfs_mount *mp, unsigned int mask);
void xfs_fs_mark_healthy(struct xfs_mount *mp, unsigned int mask);
unsigned int xfs_fs_measure_sickness(struct xfs_mount *mp);

void xfs_rt_mark_sick(struct xfs_mount *mp, unsigned int mask);
void xfs_rt_mark_healthy(struct xfs_mount *mp, unsigned int mask);
unsigned int xfs_rt_measure_sickness(struct xfs_mount *mp);

void xfs_ag_mark_sick(struct xfs_perag *pag, unsigned int mask);
void xfs_ag_mark_healthy(struct xfs_perag *pag, unsigned int mask);
unsigned int xfs_ag_measure_sickness(struct xfs_perag *pag);

void xfs_inode_mark_sick(struct xfs_inode *ip, unsigned int mask);
void xfs_inode_mark_healthy(struct xfs_inode *ip, unsigned int mask);
unsigned int xfs_inode_measure_sickness(struct xfs_inode *ip);

/* Now some helpers. */

static inline bool
xfs_fs_is_sick(struct xfs_mount *mp, unsigned int mask)
{
	return (xfs_fs_measure_sickness(mp) & mask) != 0;
}

static inline bool
xfs_rt_is_sick(struct xfs_mount *mp, unsigned int mask)
{
	return (xfs_rt_measure_sickness(mp) & mask) != 0;
}

static inline bool
xfs_ag_is_sick(struct xfs_perag *pag, unsigned int mask)
{
	return (xfs_ag_measure_sickness(pag) & mask) != 0;
}

static inline bool
xfs_inode_is_sick(struct xfs_inode *ip, unsigned int mask)
{
	return (xfs_inode_measure_sickness(ip) & mask) != 0;
}

static inline bool
xfs_fs_healthy(struct xfs_mount *mp)
{
	return xfs_fs_measure_sickness(mp) == 0;
}

static inline bool
xfs_rt_healthy(struct xfs_mount *mp)
{
	return xfs_rt_measure_sickness(mp) == 0;
}

static inline bool
xfs_ag_healthy(struct xfs_perag *pag)
{
	return xfs_ag_measure_sickness(pag) == 0;
}

static inline bool
xfs_inode_healthy(struct xfs_inode *ip)
{
	return xfs_inode_measure_sickness(ip) == 0;
}

#endif	/* __XFS_HEALTH_H__ */
