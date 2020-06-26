/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_RTRMAP_BTREE_H__
#define	__XFS_RTRMAP_BTREE_H__

struct xfs_buf;
struct xfs_btree_cur;
struct xfs_mount;
struct xbtree_ifakeroot;

/* rmaps only exist on crc enabled filesystems */
#define XFS_RTRMAP_BLOCK_LEN	XFS_BTREE_LBLOCK_CRC_LEN

struct xfs_btree_cur *xfs_rtrmapbt_init_cursor(struct xfs_mount *mp,
				struct xfs_trans *tp, struct xfs_inode *ip);
struct xfs_btree_cur *xfs_rtrmapbt_stage_cursor(struct xfs_mount *mp,
		struct xfs_inode *ip, struct xbtree_ifakeroot *ifake);
void xfs_rtrmapbt_commit_staged_btree(struct xfs_btree_cur *cur,
		struct xfs_trans *tp);
int xfs_rtrmapbt_root_maxrecs(int blocklen, bool leaf);
int xfs_rtrmapbt_maxrecs(int blocklen, bool leaf);
void xfs_rtrmapbt_compute_maxlevels(struct xfs_mount *mp);

/*
 * Record, key, and pointer address macros for btree blocks.
 *
 * (note that some of these may appear unused, but they are used in userspace)
 */
static inline struct xfs_rtrmap_rec *
xfs_rtrmap_rec_addr(
	struct xfs_btree_block	*block,
	unsigned int		index)
{
	return (struct xfs_rtrmap_rec *)
		((char *)block + XFS_RTRMAP_BLOCK_LEN +
		 (index - 1) * sizeof(struct xfs_rtrmap_rec));
}

static inline struct xfs_rtrmap_key *
xfs_rtrmap_key_addr(
	struct xfs_btree_block	*block,
	unsigned int		index)
{
	return (struct xfs_rtrmap_key *)
		((char *)block + XFS_RTRMAP_BLOCK_LEN +
		 (index - 1) * 2 * sizeof(struct xfs_rtrmap_key));
}

static inline struct xfs_rtrmap_key *
xfs_rtrmap_high_key_addr(
	struct xfs_btree_block	*block,
	unsigned int		index)
{
	return (struct xfs_rtrmap_key *)
		((char *)block + XFS_RTRMAP_BLOCK_LEN +
		 sizeof(struct xfs_rtrmap_key) +
		 (index - 1) * 2 * sizeof(struct xfs_rtrmap_key));
}

static inline xfs_rtrmap_ptr_t *
xfs_rtrmap_ptr_addr(
	struct xfs_btree_block	*block,
	unsigned int		index,
	unsigned int		maxrecs)
{
	return (xfs_rtrmap_ptr_t *)
		((char *)block + XFS_RTRMAP_BLOCK_LEN +
		 maxrecs * 2 * sizeof(struct xfs_rtrmap_key) +
		 (index - 1) * sizeof(xfs_rtrmap_ptr_t));
}

/* Macros for handling the inode root */

static inline struct xfs_rtrmap_rec *
xfs_rtrmap_root_rec_addr(
	struct xfs_rtrmap_root	*block,
	unsigned int		index)
{
	return (struct xfs_rtrmap_rec *)
		((char *)(block + 1) +
		 (index - 1) * sizeof(struct xfs_rtrmap_rec));
}

static inline struct xfs_rtrmap_key *
xfs_rtrmap_root_key_addr(
	struct xfs_rtrmap_root	*block,
	unsigned int		index)
{
	return (struct xfs_rtrmap_key *)
		((char *)(block + 1) +
		 (index - 1) * 2 * sizeof(struct xfs_rtrmap_key));
}

static inline xfs_rtrmap_ptr_t *
xfs_rtrmap_root_ptr_addr(
	struct xfs_rtrmap_root	*block,
	unsigned int		index,
	unsigned int		maxrecs)
{
	return (xfs_rtrmap_ptr_t *)
		((char *)(block + 1) +
		 maxrecs * 2 * sizeof(struct xfs_rtrmap_key) +
		 (index - 1) * sizeof(xfs_rtrmap_ptr_t));
}

static inline xfs_rtrmap_ptr_t *
xfs_rtrmap_broot_ptr_addr(
	struct xfs_btree_block	*bb,
	unsigned int		index,
	unsigned int		block_size)
{
	return xfs_rtrmap_ptr_addr(bb, index,
			xfs_rtrmapbt_maxrecs(block_size, false));
}

static inline size_t
xfs_rtrmap_broot_space_calc(
	unsigned int		nrecs,
	unsigned int		level)
{
	size_t			sz = XFS_RTRMAP_BLOCK_LEN;

	if (level > 0)
		return sz + nrecs * (2 * sizeof(struct xfs_rtrmap_key) +
					 sizeof(xfs_rtrmap_ptr_t));
	return sz + nrecs * sizeof(struct xfs_rtrmap_rec);
}

static inline size_t
xfs_rtrmap_broot_space(struct xfs_rtrmap_root *bb)
{
	return xfs_rtrmap_broot_space_calc(be16_to_cpu(bb->bb_numrecs),
			be16_to_cpu(bb->bb_level));
}

static inline size_t
xfs_rtrmap_root_space_calc(
	unsigned int		nrecs,
	unsigned int		level)
{
	size_t			sz = sizeof(struct xfs_rtrmap_root);

	if (level > 0)
		return sz + nrecs * (2 * sizeof(struct xfs_rtrmap_key) +
					 sizeof(xfs_rtrmap_ptr_t));
	return sz + nrecs * sizeof(struct xfs_rtrmap_rec);
}

static inline size_t
xfs_rtrmap_root_space(struct xfs_btree_block *bb)
{
	return xfs_rtrmap_root_space_calc(be16_to_cpu(bb->bb_numrecs),
			be16_to_cpu(bb->bb_level));
}

int xfs_iformat_rtrmap(struct xfs_inode *ip, struct xfs_dinode *dip);
void xfs_iflush_rtrmap(struct xfs_inode *ip, struct xfs_dinode *dip);

struct xfs_imeta_end;

int xfs_rtrmapbt_create(struct xfs_trans **tpp, struct xfs_imeta_end *ic,
		struct xfs_inode **ipp);

#endif	/* __XFS_RTRMAP_BTREE_H__ */
