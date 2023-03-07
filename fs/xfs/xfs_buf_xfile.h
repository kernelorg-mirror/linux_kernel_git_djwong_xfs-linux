// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_BUF_XFILE_H__
#define __XFS_BUF_XFILE_H__

#ifdef CONFIG_XFS_IN_MEMORY_FILE
int xfile_buf_ioapply(struct xfs_buf *bp);
int xfile_alloc_buftarg(struct xfs_mount *mp, const char *descr,
		struct xfs_buftarg **btpp);
void xfile_free_buftarg(struct xfs_buftarg *btp);
xfs_daddr_t xfile_buftarg_nr_sectors(struct xfs_buftarg *btp);
#else
# define xfile_buf_ioapply(bp)			(-EOPNOTSUPP)
# define xfile_buftarg_nr_sectors(btp)		(0)
#endif /* CONFIG_XFS_IN_MEMORY_FILE */

#endif /* __XFS_BUF_XFILE_H__ */
