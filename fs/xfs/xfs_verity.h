/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 Red Hat, Inc.
 */
#ifndef __XFS_VERITY_H__
#define __XFS_VERITY_H__

#include "xfs.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include <linux/fsverity.h>

#define XFS_VERITY_DESCRIPTOR_NAME "vdesc"
#define XFS_VERITY_DESCRIPTOR_NAME_LEN 5

static inline bool
xfs_verity_merkle_block(
		struct xfs_da_args *args)
{
	if (!(args->attr_filter & XFS_ATTR_VERITY))
		return false;

	return true;
}

#ifdef CONFIG_FS_VERITY
void __xfs_verity_destroy_cache(struct xfs_inode *ip);

/*
 * Clean up the merkle tree cache while destroying an inode.  Caller must
 * ensure that there are no other threads accessing i_merkle_blocks.
 */
static inline void
xfs_verity_destroy_cache(
	struct xfs_inode	*ip)
{
	if (ip->i_merkle_blocks)
		__xfs_verity_destroy_cache(ip);
}

extern const struct fsverity_operations xfs_verity_ops;
#else
# define xfs_verity_destroy_cache(ip)	((void)0)
#endif	/* CONFIG_FS_VERITY */

#endif	/* __XFS_VERITY_H__ */
