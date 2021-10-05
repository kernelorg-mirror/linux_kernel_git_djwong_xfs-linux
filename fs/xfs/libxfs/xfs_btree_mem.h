/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_BTREE_MEM_H__
#define __XFS_BTREE_MEM_H__

struct xfbtree;

#ifdef CONFIG_XFS_ONLINE_REPAIR
struct xfs_buftarg *xfbtree_target(struct xfbtree *xfbtree);
bool xfbtree_check_ptr(struct xfs_btree_cur *cur, xfs_fileoff_t xfileoff);

void xfs_btree_mem_set_root(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *ptr, int inc);
void xfs_btree_mem_init_ptr_from_cur(struct xfs_btree_cur *cur,
		union xfs_btree_ptr *ptr);
struct xfs_btree_cur *xfs_btree_mem_dup_cursor(struct xfs_btree_cur *cur);
unsigned int xfs_btree_mem_head_nlevels(struct xfs_buf *head_bp);

struct xfbtree *xfbtree_create(struct xfs_mount *mp, xfs_btnum_t btnum,
		const struct xfs_buf_ops *b_ops, unsigned int flags,
		const char *description);
int xfbtree_alloc_block(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *start, union xfs_btree_ptr *ptr,
		int *stat);
int xfbtree_free_block(struct xfs_btree_cur *cur, struct xfs_buf *bp);
#else
static inline struct xfs_buftarg *
xfbtree_target(struct xfbtree *xfbtree)
{
	return NULL;
}

static inline bool
xfbtree_check_ptr(struct xfs_btree_cur *cur, xfs_fileoff_t xfileoff)
{
	return false;
}

static inline void
xfs_btree_mem_set_root(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *ptr, int inc)
{
}

static inline void
xfs_btree_mem_init_ptr_from_cur(struct xfs_btree_cur *cur,
		union xfs_btree_ptr *ptr)
{
}

static inline struct xfs_btree_cur *
xfs_btree_mem_dup_cursor(struct xfs_btree_cur *cur)
{
	return NULL;
}

static inline unsigned int
xfs_btree_mem_head_nlevels(struct xfs_buf *head_bp)
{
	return 0;
}

static inline struct xfbtree *
xfbtree_create(struct xfs_mount *mp, xfs_btnum_t btnum,
		const struct xfs_buf_ops *b_ops, unsigned int flags,
		const char *description)
{
	return NULL;
}

static inline int
xfbtree_alloc_block(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *start, union xfs_btree_ptr *ptr,
		int *stat)
{
	return -EINVAL;
}

static inline int
xfbtree_free_block(struct xfs_btree_cur *cur, struct xfs_buf *bp)
{
	return -EINVAL;
}
#endif /* CONFIG_XFS_ONLINE_REPAIR */

/* btree has long pointers */
#define XFBTREE_CREATE_LONG_PTRS	(1U << 0)
/* buffers should be directly mapped from memory */
#define XFBTREE_DIRECT_MAP		(1U << 1)

#endif /* __XFS_BTREE_MEM_H__ */
