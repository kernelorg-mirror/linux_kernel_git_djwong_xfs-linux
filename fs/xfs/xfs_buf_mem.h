// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_BUF_MEM_H__
#define __XFS_BUF_MEM_H__

#define XFBUF_BLOCKSIZE			(PAGE_SIZE)
#define XFBUF_BLOCKSHIFT		(PAGE_SHIFT)

struct xfbuf {
	struct file		*file;
	struct xfs_buftarg	*target;
	struct xfs_buf_cache	bcache;
};

#ifdef CONFIG_XFS_MEMORY_BUFS
int xfbuf_alloc(struct xfs_mount *mp, const char *descr, struct xfbuf **xfbp);
void xfbuf_free(struct xfbuf *xfb);

int xfbuf_map_pages(struct xfs_buf *bp, xfs_buf_flags_t flags);
void xfbuf_unmap_pages(struct xfs_buf *bp);
bool xfbuf_verify_daddr(struct xfs_buftarg *btp, xfs_daddr_t daddr);
void xfbuf_trans_bdetach(struct xfs_trans *tp, struct xfs_buf *bp,
		struct list_head *buf_list);
#define xfbuf_bwrite(...)		(0)
#else
# define xfbuf_map_pages(...)		(-ENOMEM)
# define xfbuf_unmap_pages(...)		((void)0)
# define xfbuf_verify_daddr(...)	(false)
#endif /* CONFIG_XFS_MEMORY_BUFS */

#endif /* __XFS_BUF_MEM_H__ */
