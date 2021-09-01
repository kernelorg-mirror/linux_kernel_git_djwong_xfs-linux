/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * FS_IOC_GETFSREFCOUNTS ioctl infrastructure.
 *
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 *
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef _LINUX_FSREFCOUNTS_H
#define _LINUX_FSREFCOUNTS_H

#include <linux/types.h>

/*
 *	Structure for FS_IOC_GETFSREFCOUNTS.
 *
 *	The memory layout for this call are the scalar values defined in
 *	struct fsrefs_head, followed by two struct fsrefs that describe the
 *	lower and upper bound of mappings to return, followed by an array of
 *	struct fsrefs mappings.
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
struct fsrefs {
	__u32		fcr_device;	/* device id */
	__u32		fcr_flags;	/* mapping flags */
	__u64		fcr_physical;	/* device offset of segment */
	__u64		fcr_owners;	/* number of owners */
	__u64		fcr_length;	/* length of segment */
	__u64		fcr_reserved[4];	/* must be zero */
};

struct fsrefs_head {
	__u32		fch_iflags;	/* control flags */
	__u32		fch_oflags;	/* output flags */
	__u32		fch_count;	/* # of entries in array incl. input */
	__u32		fch_entries;	/* # of entries filled in (output). */
	__u64		fch_reserved[6];	/* must be zero */

	struct fsrefs	fch_keys[2];	/* low and high keys for the mapping search */
	struct fsrefs	fch_recs[];	/* returned records */
};

/* Size of an fsrefs_head with room for nr records. */
static inline unsigned long long
fsrefs_sizeof(
	unsigned int	nr)
{
	return sizeof(struct fsrefs_head) + nr * sizeof(struct fsrefs);
}

/* Start the next fsrefs query at the end of the current query results. */
static inline void
fsrefs_advance(
	struct fsrefs_head	*head)
{
	head->fch_keys[0] = head->fch_recs[head->fch_entries - 1];
}

/*	fch_iflags values - set by FS_IOC_GETFSREFCOUNTS caller in the header. */
/* no flags defined yet */
#define FCH_IF_VALID		0

/*	fch_oflags values - returned in the header segment only. */
#define FCH_OF_DEV_T		0x1	/* fcr_device values will be dev_t */

/*	fcr_flags values - returned for each non-header segment */
#define FCR_OF_LAST		(1U << 0)	/* segment is the last in the dataset */

/* XXX stealing XFS_IOC_GETBIOSIZE */
#define FS_IOC_GETFSREFCOUNTS		_IOWR('X', 47, struct fsrefs_head)

#endif /* _LINUX_FSREFCOUNTS_H */
