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
void xfs_verity_cache_init(struct xfs_inode *ip);
void xfs_verity_cache_drop(struct xfs_inode *ip);
void xfs_verity_cache_destroy(struct xfs_inode *ip);

unsigned long xfs_verity_cache_shrink_scan(struct xfs_inode *ip,
		unsigned long nr_to_scan);
unsigned long xfs_verity_cache_shrink_count(struct xfs_inode *ip);

extern const struct fsverity_operations xfs_verity_ops;
#else
# define xfs_verity_cache_init(ip)		((void)0)
# define xfs_verity_cache_drop(ip)		((void)0)
# define xfs_verity_cache_destroy(ip)		((void)0)
#endif	/* CONFIG_FS_VERITY */

#endif	/* __XFS_VERITY_H__ */
