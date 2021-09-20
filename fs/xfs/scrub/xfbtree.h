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
	__be64				mh_root;
};

#define XFS_BTREE_MEM_HEAD_MAGIC		0x4341544D	/* "CATM" */

/* in-memory btree header is always block 0 in the backing store */
#define XFS_BTREE_MEM_HEAD_DADDR		0

int xfs_btree_mem_head_read_buf(struct xfs_buftarg *btp, struct xfs_trans *tp,
		struct xfs_buf **head_bpp);

/* xfile-backed in-memory btrees */

struct xfbtree {
	struct xfs_buftarg		*target;
};

#endif /* XFS_SCRUB_XFBTREE_H__ */
