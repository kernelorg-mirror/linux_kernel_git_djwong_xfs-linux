// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_FSREFS_H__
#define __XFS_FSREFS_H__

struct xfs_getfsrefs;

/* internal fsrefs representation */
struct xfs_fsrefs {
	dev_t		fcr_device;	/* device id */
	uint32_t	fcr_flags;	/* mapping flags */
	uint64_t	fcr_physical;	/* device offset of segment */
	uint64_t	fcr_owners;	/* number of owners */
	xfs_filblks_t	fcr_length;	/* length of segment, blocks */
};

struct xfs_fsrefs_head {
	uint32_t	fch_iflags;	/* control flags */
	uint32_t	fch_oflags;	/* output flags */
	unsigned int	fch_count;	/* # of entries in array incl. input */
	unsigned int	fch_entries;	/* # of entries filled in (output). */

	struct xfs_fsrefs fch_keys[2];	/* low and high keys */
};

int xfs_ioc_getfsrefcounts(struct xfs_inode *ip,
		struct xfs_getfsrefs_head __user *arg);

#endif /* __XFS_FSREFS_H__ */
