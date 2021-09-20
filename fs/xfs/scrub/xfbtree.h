/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_SCRUB_XFBTREE_H__
#define XFS_SCRUB_XFBTREE_H__

/* Root block for an in-memory btree. */
struct xfs_btree_mem_head {
	__be32				mh_magic;
	__be32				mh_nlevels;
	__be64				mh_owner;
	__be64				mh_root;
	uuid_t				mh_uuid;
};

#define XFS_BTREE_MEM_HEAD_MAGIC	0x4341544D	/* "CATM" */

/* xfile-backed in-memory btrees */

struct xfbtree {
	/* buffer cache target for this in-memory btree */
	struct xfs_buftarg		*target;

	/* Swappable memory file backing this btree. */
	struct xfile			*xfile;

	/* Bitmap of free space from pos to used */
	struct xbitmap			*freespace;

	/* Number of xfile blocks actually used by this xfbtree. */
	xfileoff_t			xf_used;

	/* Owner of this btree. */
	unsigned long long		owner;

	/* Minimum and maximum records per block. */
	unsigned int			maxrecs[2];
	unsigned int			minrecs[2];
};

/* The head of the in-memory btree is always at block 0 */
#define XFBTREE_HEAD_BLOCK		0

/* in-memory btrees are always created with an empty leaf block at block 1 */
#define XFBTREE_INIT_LEAF_BLOCK		1

int xfbtree_head_read_buf(struct xfbtree *xfbt, struct xfs_trans *tp,
		struct xfs_buf **bpp);

void xfbtree_destroy(struct xfbtree *xfbt);
int xfbtree_trans_commit(struct xfbtree *xfbt, struct xfs_trans *tp);
void xfbtree_trans_cancel(struct xfbtree *xfbt, struct xfs_trans *tp);

#endif /* XFS_SCRUB_XFBTREE_H__ */
