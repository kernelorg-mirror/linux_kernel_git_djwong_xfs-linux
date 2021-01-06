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

/*
 * Initial ids, link count, device number, and mode of a new inode.
 *
 * Due to our only partial reliance on the VFS to propagate uid and gid values
 * according to accepted Unix behaviors, callers must initialize uid to
 * current_fsuid() and gid to current_fsgid() to get the standard inheritance
 * behaviors when XFS_MOUNT_GRPID is set.  To override the default ids, use the
 * FORCE flags defined below.
 */
struct xfs_ialloc_args {
	struct xfs_inode	*pip;	/* parent inode or null */

	kuid_t			uid;
	kgid_t			gid;
	prid_t			prid;

	xfs_nlink_t		nlink;
	dev_t			rdev;

	umode_t			mode;

#define XFS_IALLOC_ARGS_FORCE_UID	(1 << 0)
#define XFS_IALLOC_ARGS_FORCE_GID	(1 << 1)
	uint16_t		flags;
};

#endif /* __XFS_INODE_UTIL_H__ */
