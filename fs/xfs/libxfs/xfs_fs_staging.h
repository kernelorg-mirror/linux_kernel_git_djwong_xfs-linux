/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_FS_STAGING_H__
#define __XFS_FS_STAGING_H__

/*
 * Experimental system calls, ioctls and data structures supporting them.
 * Nothing in here should be considered part of a stable interface of any kind.
 *
 * If you add an ioctl here, please leave a comment in xfs_fs.h marking it
 * reserved.  If you promote anything out of this file, please leave a comment
 * explaining where it went.
 */

/*
 * Output for XFS_IOC_RTGROUP_GEOMETRY
 */
struct xfs_rtgroup_geometry {
	__u32 rg_number;	/* i/o: rtgroup number */
	__u32 rg_length;	/* o: length in blocks */
	__u32 rg_sick;		/* o: sick things in ag */
	__u32 rg_checked;	/* o: checked metadata in ag */
	__u32 rg_flags;		/* i/o: flags for this ag */
	__u32 rg_pad;		/* o: zero */
	__u64 rg_reserved[13];	/* o: zero */
};
#define XFS_RTGROUP_GEOM_SICK_SUPER	(1 << 0)  /* superblock */
#define XFS_RTGROUP_GEOM_SICK_BITMAP	(1 << 1)  /* rtbitmap for this group */
#define XFS_RTGROUP_GEOM_SICK_RMAPBT	(1 << 2)  /* reverse mappings */
#define XFS_RTGROUP_GEOM_SICK_REFCNTBT	(1 << 3)  /* reference counts */

#define XFS_IOC_RTGROUP_GEOMETRY _IOWR('X', 63, struct xfs_rtgroup_geometry)

/*
 *	Structure for XFS_IOC_GETFSREFCOUNTS.
 *
 *	The memory layout for this call are the scalar values defined in struct
 *	xfs_getfsrefs_head, followed by two struct xfs_getfsrefs that describe
 *	the lower and upper bound of mappings to return, followed by an array
 *	of struct xfs_getfsrefs mappings.
 *
 *	fch_iflags control the output of the call, whereas fch_oflags report
 *	on the overall record output.  fch_count should be set to the length
 *	of the fch_recs array, and fch_entries will be set to the number of
 *	entries filled out during each call.  If fch_count is zero, the number
 *	of refcount mappings will be returned in fch_entries, though no
 *	mappings will be returned.  fch_reserved must be set to zero.
 *
 *	The two elements in the fch_keys array are used to constrain the
 *	output.  The first element in the array should represent the lowest
 *	disk mapping ("low key") that the user wants to learn about.  If this
 *	value is all zeroes, the filesystem will return the first entry it
 *	knows about.  For a subsequent call, the contents of
 *	fsrefs_head.fch_recs[fsrefs_head.fch_count - 1] should be copied into
 *	fch_keys[0] to have the kernel start where it left off.
 *
 *	The second element in the fch_keys array should represent the highest
 *	disk mapping ("high key") that the user wants to learn about.  If this
 *	value is all ones, the filesystem will not stop until it runs out of
 *	mapping to return or runs out of space in fch_recs.
 *
 *	fcr_device can be either a 32-bit cookie representing a device, or a
 *	32-bit dev_t if the FCH_OF_DEV_T flag is set.  fcr_physical and
 *	fcr_length are expressed in units of bytes.  fcr_owners is the number
 *	of owners.
 */
struct xfs_getfsrefs {
	__u32		fcr_device;	/* device id */
	__u32		fcr_flags;	/* mapping flags */
	__u64		fcr_physical;	/* device offset of segment */
	__u64		fcr_owners;	/* number of owners */
	__u64		fcr_length;	/* length of segment */
	__u64		fcr_reserved[4];	/* must be zero */
};

struct xfs_getfsrefs_head {
	__u32		fch_iflags;	/* control flags */
	__u32		fch_oflags;	/* output flags */
	__u32		fch_count;	/* # of entries in array incl. input */
	__u32		fch_entries;	/* # of entries filled in (output). */
	__u64		fch_reserved[6];	/* must be zero */

	struct xfs_getfsrefs	fch_keys[2];	/* low and high keys for the mapping search */
	struct xfs_getfsrefs	fch_recs[];	/* returned records */
};

/* Size of an fsrefs_head with room for nr records. */
static inline unsigned long long
xfs_getfsrefs_sizeof(
	unsigned int	nr)
{
	return sizeof(struct xfs_getfsrefs_head) + nr * sizeof(struct xfs_getfsrefs);
}

/* Start the next fsrefs query at the end of the current query results. */
static inline void
xfs_getfsrefs_advance(
	struct xfs_getfsrefs_head	*head)
{
	head->fch_keys[0] = head->fch_recs[head->fch_entries - 1];
}

/*	fch_iflags values - set by XFS_IOC_GETFSREFCOUNTS caller in the header. */
/* no flags defined yet */
#define FCH_IF_VALID		0

/*	fch_oflags values - returned in the header segment only. */
#define FCH_OF_DEV_T		(1U << 0)	/* fcr_device values will be dev_t */

/*	fcr_flags values - returned for each non-header segment */
#define FCR_OF_LAST		(1U << 0)	/* segment is the last in the dataset */

/* XXX stealing XFS_IOC_GETBIOSIZE */
#define XFS_IOC_GETFSREFCOUNTS		_IOWR('X', 47, struct xfs_getfsrefs_head)

/* map free space to file */

struct xfs_map_freesp {
	__s64	offset;		/* disk address to map, in bytes */
	__s64	len;		/* length in bytes */
	__u64	flags;		/* must be zero */
	__u64	pad;		/* must be zero */
};

/*
 * XFS_IOC_MAP_FREESP maps all the free physical space in the filesystem into
 * the file at the same offsets.  This ioctl requires CAP_SYS_ADMIN.
 */
#define XFS_IOC_MAP_FREESP	_IOWR('X', 64, struct xfs_map_freesp)

/* Turn off fs-verity */
#define FS_IOC_DISABLE_VERITY	_IO('f', 133)

struct xfs_health_monitor {
	__u64	flags;		/* flags */
	__u8	format;		/* output format */
	__u8	pad1[7];	/* zeroes */
	__u64	pad2[2];	/* zeroes */
};

/* Return all health status events, not just deltas */
#define XFS_HEALTH_MONITOR_VERBOSE	(1ULL << 0)

#define XFS_HEALTH_MONITOR_ALL		(XFS_HEALTH_MONITOR_VERBOSE)

/* Return events in JSON format */
#define XFS_HEALTH_MONITOR_FMT_JSON	(1)

/* Monitor for health events. */
#define XFS_IOC_HEALTH_MONITOR		_IOR ('X', 48, struct xfs_health_monitor)

#endif /* __XFS_FS_STAGING_H__ */
