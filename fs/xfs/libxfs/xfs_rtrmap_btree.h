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

struct xfs_btree_cur *xfs_rtrmapbt_init_cursor(struct xfs_mount *mp,
				struct xfs_trans *tp, struct xfs_inode *ip);
int xfs_rtrmapbt_maxrecs(int blocklen, bool leaf);
void xfs_rtrmapbt_compute_maxlevels(struct xfs_mount *mp);

#endif	/* __XFS_RTRMAP_BTREE_H__ */
