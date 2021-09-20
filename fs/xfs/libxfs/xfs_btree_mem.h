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
#endif /* CONFIG_XFS_ONLINE_REPAIR */

#endif /* __XFS_BTREE_MEM_H__ */
