// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SCRUB_BITMAP_H__
#define __XFS_SCRUB_BITMAP_H__

#include <linux/interval_tree.h>

struct xbitmap {
	struct rb_root_cached	root;
};

void xbitmap_init(struct xbitmap *bitmap);
void xbitmap_destroy(struct xbitmap *bitmap);

#define for_each_xbitmap_extent(itn, n, bitmap) \
	rbtree_postorder_for_each_entry_safe((itn), (n), \
			&(bitmap)->root.rb_root, rb)

#define for_each_xbitmap_block(b, itn, n, bitmap) \
	for_each_xbitmap_extent((itn), (n), (bitmap)) \
		for ((b) = (itn)->start; (b) <= (itn)->last; (b)++)

void xbitmap_clear(struct xbitmap *bitmap, uint64_t start, uint64_t len);
int xbitmap_set(struct xbitmap *bitmap, uint64_t start, uint64_t len);
void xbitmap_disunion(struct xbitmap *bitmap, struct xbitmap *sub);
int xbitmap_set_btcur_path(struct xbitmap *bitmap,
		struct xfs_btree_cur *cur);
int xbitmap_set_btblocks(struct xbitmap *bitmap,
		struct xfs_btree_cur *cur);
bool xbitmap_empty(struct xbitmap *bitmap);

#endif	/* __XFS_SCRUB_BITMAP_H__ */
