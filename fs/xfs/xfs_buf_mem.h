// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_BUF_MEM_H__
#define __XFS_BUF_MEM_H__

extern struct kmem_cache		*xfs_self_buftarg_cache;

#define XMBUF_BLOCKSIZE			(PAGE_SIZE)
#define XMBUF_BLOCKSHIFT		(PAGE_SHIFT)

#define XBC_BLOCKSIZE			(BBSIZE)
#define XBC_BLOCKSHIFT			(BBSHIFT)

#ifdef CONFIG_XFS_MEMORY_BUFS
static inline bool xfs_buftarg_is_mem(const struct xfs_buftarg *btp)
{
	return btp->bt_bdev == NULL && btp->bt_file != NULL;
}

int xmbuf_alloc(struct xfs_mount *mp, const char *descr,
		struct xfs_buftarg **btpp);
void xmbuf_free(struct xfs_buftarg *btp);

int xmbuf_map_page(struct xfs_buf *bp);
void xmbuf_unmap_page(struct xfs_buf *bp);
bool xmbuf_verify_daddr(struct xfs_buftarg *btp, xfs_daddr_t daddr);
void xmbuf_trans_bdetach(struct xfs_trans *tp, struct xfs_buf *bp);
int xmbuf_finalize(struct xfs_buf *bp);

static inline bool xfs_buftarg_is_blobcache(const struct xfs_buftarg *btp)
{
	return btp->bt_bdev == NULL && btp->bt_file == NULL;
}

typedef __s64 xblobc_key_t;

int xblobc_alloc(struct xfs_mount *mp, dev_t cookie, const char *descr,
		struct xfs_buftarg **btpp);
void xblobc_free(struct xfs_buftarg *btp);

int xblobc_peek(struct xfs_buftarg *target, xblobc_key_t key, size_t bytecount,
		struct xfs_buf **bpp);
int xblobc_get(struct xfs_buftarg *target, xblobc_key_t key, size_t bytecount,
		struct xfs_buf **bpp);
void xblobc_store(struct xfs_buf *bp, void *data, size_t datalen);
#else
# define xfs_buftarg_is_mem(...)	(false)
# define xmbuf_map_page(...)		(-ENOMEM)
# define xmbuf_unmap_page(...)		((void)0)
# define xmbuf_verify_daddr(...)	(false)

# define xfs_buftarg_is_blobcache(...)	(false)
#endif /* CONFIG_XFS_MEMORY_BUFS */

#endif /* __XFS_BUF_MEM_H__ */
