/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_BTREE_MEM_H__
#define __XFS_BTREE_MEM_H__

struct xfbtree;

struct xfbtree_config {
	/* Buffer ops for the btree root block */
	const struct xfs_btree_ops	*btree_ops;

	/* Buffer target for the xfile backing this btree. */
	struct xfs_buftarg		*target;

	/* Owner of this btree. */
	unsigned long long		owner;

	/* Btree type number */
	xfs_btnum_t			btnum;

	/* XFBTREE_CREATE_* flags */
	unsigned int			flags;
};

/* btree has long pointers */
#define XFBTREE_CREATE_LONG_PTRS	(1U << 0)

#ifdef CONFIG_XFS_BTREE_IN_XFILE
unsigned int xfs_btree_mem_head_nlevels(struct xfs_buf *head_bp);

struct xfs_buftarg *xfbtree_target(struct xfbtree *xfbtree);
int xfbtree_check_ptr(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *ptr, int index, int level);
xfs_daddr_t xfbtree_ptr_to_daddr(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *ptr);
void xfbtree_buf_to_ptr(struct xfs_btree_cur *cur, struct xfs_buf *bp,
		union xfs_btree_ptr *ptr);

unsigned int xfbtree_bbsize(void);

void xfbtree_set_root(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *ptr, int inc);
void xfbtree_init_ptr_from_cur(struct xfs_btree_cur *cur,
		union xfs_btree_ptr *ptr);
struct xfs_btree_cur *xfbtree_dup_cursor(struct xfs_btree_cur *cur);
bool xfbtree_verify_xfileoff(struct xfs_btree_cur *cur,
		unsigned long long xfoff);
xfs_failaddr_t xfbtree_check_block_owner(struct xfs_btree_cur *cur,
		struct xfs_btree_block *block);
unsigned long long xfbtree_owner(struct xfs_btree_cur *cur);
xfs_failaddr_t xfbtree_lblock_verify(struct xfs_buf *bp, unsigned int max_recs);
xfs_failaddr_t xfbtree_sblock_verify(struct xfs_buf *bp, unsigned int max_recs);
unsigned long long xfbtree_buf_to_xfoff(struct xfs_btree_cur *cur,
		struct xfs_buf *bp);

int xfbtree_get_minrecs(struct xfs_btree_cur *cur, int level);
int xfbtree_get_maxrecs(struct xfs_btree_cur *cur, int level);

int xfbtree_create(struct xfs_mount *mp, const struct xfbtree_config *cfg,
		struct xfbtree **xfbtreep);
int xfbtree_alloc_block(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *start, union xfs_btree_ptr *ptr,
		int *stat);
int xfbtree_free_block(struct xfs_btree_cur *cur, struct xfs_buf *bp);
#else
static inline unsigned int xfs_btree_mem_head_nlevels(struct xfs_buf *head_bp)
{
	return 0;
}

static inline struct xfs_buftarg *
xfbtree_target(struct xfbtree *xfbtree)
{
	return NULL;
}

static inline int
xfbtree_check_ptr(struct xfs_btree_cur *cur, const union xfs_btree_ptr *ptr,
		  int index, int level)
{
	return 0;
}

static inline xfs_daddr_t
xfbtree_ptr_to_daddr(struct xfs_btree_cur *cur, const union xfs_btree_ptr *ptr)
{
	return 0;
}

static inline void
xfbtree_buf_to_ptr(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp,
	union xfs_btree_ptr	*ptr)
{
	memset(ptr, 0xFF, sizeof(*ptr));
}

static inline unsigned int xfbtree_bbsize(void)
{
	return 0;
}

#define xfbtree_set_root			NULL
#define xfbtree_init_ptr_from_cur		NULL
#define xfbtree_dup_cursor			NULL
#define xfbtree_get_minrecs			NULL
#define xfbtree_get_maxrecs			NULL
#define xfbtree_alloc_block			NULL
#define xfbtree_free_block			NULL
#define xfbtree_verify_xfileoff(cur, xfoff)	(false)
#define xfbtree_check_block_owner(cur, block)	NULL
#define xfbtree_owner(cur)			(0ULL)
#define xfbtree_buf_to_xfoff(cur, bp)		(-1)

static inline int
xfbtree_create(struct xfs_mount *mp, const struct xfbtree_config *cfg,
		struct xfbtree **xfbtreep)
{
	return -EOPNOTSUPP;
}

#endif /* CONFIG_XFS_BTREE_IN_XFILE */

#endif /* __XFS_BTREE_MEM_H__ */
