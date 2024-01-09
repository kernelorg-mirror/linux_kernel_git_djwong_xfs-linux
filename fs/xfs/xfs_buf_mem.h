// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_BUF_MEM_H__
#define __XFS_BUF_MEM_H__

#define XMBUF_BLOCKSIZE			(PAGE_SIZE)
#define XMBUF_BLOCKSHIFT		(PAGE_SHIFT)

struct xmbuf {
	struct file		*file;
	struct xfs_buftarg	target;
	struct xfs_buf_cache	bcache;
};

#ifdef CONFIG_XFS_MEMORY_BUFS
int xmbuf_alloc(struct xfs_mount *mp, const char *descr, struct xmbuf **xmbp);
void xmbuf_free(struct xmbuf *xmb);

int xmbuf_map_page(struct xfs_buf *bp);
void xmbuf_unmap_page(struct xfs_buf *bp);
#else
# define xmbuf_map_page(...)		(-ENOMEM)
# define xmbuf_unmap_page(...)		((void)0)
#endif /* CONFIG_XFS_MEMORY_BUFS */

#endif /* __XFS_BUF_MEM_H__ */
