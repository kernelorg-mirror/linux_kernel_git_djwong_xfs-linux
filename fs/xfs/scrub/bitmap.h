// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SCRUB_BITMAP_H__
#define __XFS_SCRUB_BITMAP_H__

struct xbitmap_range {
	struct list_head	list;
	uint64_t		start;
	uint64_t		len;
};

struct xbitmap {
	struct list_head	list;
};

void xbitmap_init(struct xbitmap *bitmap);
void xbitmap_destroy(struct xbitmap *bitmap);

int xbitmap_set(struct xbitmap *bitmap, uint64_t start, uint64_t len);
int xbitmap_disunion(struct xbitmap *bitmap, struct xbitmap *sub);
int xbitmap_set_btcur_path(struct xbitmap *bitmap,
		struct xfs_btree_cur *cur);
int xbitmap_set_btblocks(struct xbitmap *bitmap,
		struct xfs_btree_cur *cur);
uint64_t xbitmap_hweight(struct xbitmap *bitmap);

typedef int (*xbitmap_walk_run_fn)(uint64_t start, uint64_t len, void *priv);
int xbitmap_walk_set_runs(struct xbitmap *bitmap, xbitmap_walk_run_fn fn,
		void *priv);

typedef int (*xbitmap_walk_bit_fn)(uint64_t bit, void *priv);
int xbitmap_walk_set_bits(struct xbitmap *bitmap, xbitmap_walk_bit_fn fn,
		void *priv);

#endif	/* __XFS_SCRUB_BITMAP_H__ */
