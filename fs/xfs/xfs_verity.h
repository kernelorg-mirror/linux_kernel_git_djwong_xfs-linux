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

#ifdef CONFIG_FS_VERITY
void __xfs_verity_destroy_cache(struct xfs_inode *ip);

static inline bool xfs_verity_has_cache(struct xfs_inode *ip)
{
	return ip->i_merkle_blocks != NULL;
}

static inline void
xfs_verity_destroy_cache(
	struct xfs_inode	*ip)
{
	if (xfs_verity_has_cache(ip))
		__xfs_verity_destroy_cache(ip);
}

extern const struct fsverity_operations xfs_verity_ops;
#else
# define xfs_verity_destroy_inode(ip)	((void)0)
#endif	/* CONFIG_FS_VERITY */

#endif	/* __XFS_VERITY_H__ */
