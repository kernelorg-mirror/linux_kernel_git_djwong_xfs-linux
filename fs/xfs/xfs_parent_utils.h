// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022-2023 Oracle, Inc.
 * All rights reserved.
 */
#ifndef	__XFS_PARENT_UTILS_H__
#define	__XFS_PARENT_UTILS_H__

static inline unsigned int
xfs_getparents_arraytop(
	const struct xfs_getparents	*ppi,
	unsigned int			nr)
{
	return sizeof(struct xfs_getparents) +
			(nr * sizeof(ppi->gp_offsets[0]));
}

int xfs_getparent_pointers(struct xfs_inode *ip, struct xfs_getparents *ppi);

#endif	/* __XFS_PARENT_UTILS_H__ */
