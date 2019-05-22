// SPDX-License-Identifier: GPL-2.0
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

#define XFS_PROJID_DEFAULT		0

prid_t		xfs_get_projid(struct xfs_inode *ip);
void		xfs_set_projid(struct xfs_inode *ip, prid_t projid);
prid_t		xfs_get_initial_prid(struct xfs_inode *dp);

/* Initial ids, link count, device number, and mode of a new inode. */
struct xfs_ialloc_args {
	struct xfs_inode		*pip;	/* parent inode or null */

	uint32_t			uid;
	uint32_t			gid;
	prid_t				prid;

	xfs_nlink_t			nlink;
	dev_t				rdev;

	umode_t				mode;
};

#endif /* __XFS_INODE_UTIL_H__ */
