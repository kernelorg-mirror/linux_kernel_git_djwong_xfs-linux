/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2003,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef	__XFS_INODE_UTIL_H__
#define	__XFS_INODE_UTIL_H__

uint16_t	xfs_flags2diflags(struct xfs_inode *ip, unsigned int xflags);
uint64_t	xfs_flags2diflags2(struct xfs_inode *ip, unsigned int xflags);
uint32_t	xfs_dic2xflags(uint16_t di_flags, uint64_t di_flags2,
			       bool has_attr);

prid_t		xfs_get_initial_prid(struct xfs_inode *dp);

/* Initial ids, link count, device number, and mode of a new inode. */
struct xfs_ialloc_args {
	struct xfs_inode	*pip;	/* parent inode or null */

	kuid_t			uid;
	kgid_t			gid;
	prid_t			prid;

	xfs_nlink_t		nlink;
	dev_t			rdev;

	umode_t			mode;
};

/*
 * Flags for xfs_trans_ichgtime().
 */
#define	XFS_ICHGTIME_MOD	0x1	/* data fork modification timestamp */
#define	XFS_ICHGTIME_CHG	0x2	/* inode field change timestamp */
#define	XFS_ICHGTIME_CREATE	0x4	/* inode create timestamp */
#define	XFS_ICHGTIME_ACCESS	0x8	/* last access timestamp */
void xfs_trans_ichgtime(struct xfs_trans *tp, struct xfs_inode *ip, int flags);

void xfs_inode_init(struct xfs_trans *tp, const struct xfs_ialloc_args *args,
		struct xfs_inode *ip);

/* The libxfs client must provide this group of helper functions. */

/* Initialize the incore inode. */
void xfs_setup_inode(struct xfs_inode *ip);

#endif /* __XFS_INODE_UTIL_H__ */
