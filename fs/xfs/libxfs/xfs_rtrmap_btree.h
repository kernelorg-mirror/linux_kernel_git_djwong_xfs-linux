/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_RTRMAP_BTREE_H__
#define	__XFS_RTRMAP_BTREE_H__

struct xfs_buf;
struct xfs_btree_cur;
struct xfs_mount;

/* rmaps only exist on crc enabled filesystems */
#define XFS_RTRMAP_BLOCK_LEN	XFS_BTREE_LBLOCK_CRC_LEN

/*
 * Record, key, and pointer address macros for btree blocks.
 *
 * (note that some of these may appear unused, but they are used in userspace)
 */
#define XFS_RTRMAP_REC_ADDR(block, index) \
	((struct xfs_rtrmap_rec *) \
		((char *)(block) + XFS_RTRMAP_BLOCK_LEN + \
		 (((index) - 1) * sizeof(struct xfs_rtrmap_rec))))

#define XFS_RTRMAP_KEY_ADDR(block, index) \
	((struct xfs_rtrmap_key *) \
		((char *)(block) + XFS_RTRMAP_BLOCK_LEN + \
		 ((index) - 1) * 2 * sizeof(struct xfs_rtrmap_key)))

#define XFS_RTRMAP_HIGH_KEY_ADDR(block, index) \
	((struct xfs_rtrmap_key *) \
		((char *)(block) + XFS_RTRMAP_BLOCK_LEN + \
		 sizeof(struct xfs_rtrmap_key) + \
		 ((index) - 1) * 2 * sizeof(struct xfs_rtrmap_key)))

#define XFS_RTRMAP_PTR_ADDR(block, index, maxrecs) \
	((xfs_rtrmap_ptr_t *) \
		((char *)(block) + XFS_RTRMAP_BLOCK_LEN + \
		 (maxrecs) * 2 * sizeof(struct xfs_rtrmap_key) + \
		 ((index) - 1) * sizeof(xfs_rtrmap_ptr_t)))

/* Macros for handling the inode root */

#define XFS_RTRMAP_ROOT_REC_ADDR(block, index) \
	((struct xfs_rtrmap_rec *) \
		((char *)(block) + \
		 sizeof(struct xfs_rtrmap_root) + \
		 ((index) - 1) * sizeof(struct xfs_rtrmap_rec)))

#define XFS_RTRMAP_ROOT_KEY_ADDR(block, index) \
	((struct xfs_rtrmap_key *) \
		((char *)(block) + \
		 sizeof(struct xfs_rtrmap_root) + \
		 ((index) - 1) * 2 * sizeof(struct xfs_rtrmap_key)))

#define XFS_RTRMAP_ROOT_PTR_ADDR(block, index, maxrecs) \
	((xfs_rtrmap_ptr_t *) \
		((char *)(block) + \
		 sizeof(struct xfs_rtrmap_root) + \
		 (maxrecs) * 2 * sizeof(struct xfs_rtrmap_key) + \
		 ((index) - 1) * sizeof(xfs_rtrmap_ptr_t)))

#define XFS_RTRMAP_BROOT_PTR_ADDR(bb, i, sz) \
	XFS_RTRMAP_PTR_ADDR(bb, i, xfs_rtrmapbt_maxrecs(sz, 0))

#define XFS_RTRMAP_BROOT_SPACE_CALC(nrecs, level) \
	(int)(XFS_RTRMAP_BLOCK_LEN + ((level) > 0 ? \
	       ((nrecs) * (2 * sizeof(struct xfs_rtrmap_key) + \
			   sizeof(xfs_rtrmap_ptr_t))) : \
	       ((nrecs) * sizeof(struct xfs_rtrmap_rec))))

#define XFS_RTRMAP_BROOT_SPACE(bb) \
	(XFS_RTRMAP_BROOT_SPACE_CALC(be16_to_cpu((bb)->bb_numrecs), \
				     be16_to_cpu((bb)->bb_level)))

#define XFS_RTRMAP_ROOT_SPACE_CALC(nrecs, level) \
	(int)(sizeof(struct xfs_rtrmap_root) + ((level) > 0 ? \
	       ((nrecs) * (2 * sizeof(struct xfs_rtrmap_key) + \
			   sizeof(xfs_rtrmap_ptr_t))) : \
	       ((nrecs) * sizeof(struct xfs_rtrmap_rec))))

#define XFS_RTRMAP_ROOT_SPACE(bb) \
	(XFS_RTRMAP_ROOT_SPACE_CALC(be16_to_cpu((bb)->bb_numrecs), \
				    be16_to_cpu((bb)->bb_level)))

struct xfs_btree_cur *xfs_rtrmapbt_init_cursor(struct xfs_mount *mp,
				struct xfs_trans *tp, struct xfs_inode *ip);
int xfs_rtrmapbt_maxrecs(int blocklen, bool leaf);
void xfs_rtrmapbt_compute_maxlevels(struct xfs_mount *mp);

void xfs_rtrmapbt_from_disk(struct xfs_inode *ip,
		struct xfs_rtrmap_root *dblock, int dblocklen,
		struct xfs_btree_block *rblock, int rblocklen);
void xfs_rtrmapbt_to_disk(struct xfs_mount *mp,
		struct xfs_btree_block *rblock, int rblocklen,
		struct xfs_rtrmap_root *dblock, int dblocklen);

struct xfs_imeta_end;

int xfs_rtrmapbt_create(struct xfs_trans **tpp, struct xfs_imeta_end *ic,
		struct xfs_inode **ipp);

#endif	/* __XFS_RTRMAP_BTREE_H__ */
