// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Oracle, Inc.
 * All rights reserved.
 */
#ifndef	__XFS_PARENT_UTILS_H__
#define	__XFS_PARENT_UTILS_H__

static inline unsigned int
xfs_getparents_arraytop(
	const struct xfs_pptr_info	*ppi,
	unsigned int			nr)
{
	return sizeof(struct xfs_pptr_info) +
			(nr * sizeof(ppi->pi_offsets[0]));
}

int xfs_getparent_pointers(struct xfs_inode *ip, struct xfs_pptr_info *ppi);

#endif	/* __XFS_PARENT_UTILS_H__ */
