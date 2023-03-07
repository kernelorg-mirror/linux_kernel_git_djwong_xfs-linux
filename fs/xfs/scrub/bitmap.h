// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_BITMAP_H__
#define __XFS_SCRUB_BITMAP_H__

/* u64 bitmap */

struct xbitmap {
	struct rb_root_cached	xb_root;
};

void xbitmap_init(struct xbitmap *bitmap);
void xbitmap_destroy(struct xbitmap *bitmap);

int xbitmap_clear(struct xbitmap *bitmap, uint64_t start, uint64_t len);
int xbitmap_set(struct xbitmap *bitmap, uint64_t start, uint64_t len);
int xbitmap_disunion(struct xbitmap *bitmap, struct xbitmap *sub);
uint64_t xbitmap_hweight(struct xbitmap *bitmap);

/*
 * Return codes for the bitmap iterator functions are 0 to continue iterating,
 * and non-zero to stop iterating.  Any non-zero value will be passed up to the
 * iteration caller.  The special value -ECANCELED can be used to stop
 * iteration, because neither bitmap iterator ever generates that error code on
 * its own.  Callers must not modify the bitmap while walking it.
 */
typedef int (*xbitmap_walk_fn)(uint64_t start, uint64_t len, void *priv);
int xbitmap_walk(struct xbitmap *bitmap, xbitmap_walk_fn fn,
		void *priv);

bool xbitmap_empty(struct xbitmap *bitmap);
bool xbitmap_test(struct xbitmap *bitmap, uint64_t start, uint64_t *len);

/* u32 bitmap */

struct xbitmap32 {
	struct rb_root_cached	xb_root;
};

void xbitmap32_init(struct xbitmap32 *bitmap);
void xbitmap32_destroy(struct xbitmap32 *bitmap);

int xbitmap32_clear(struct xbitmap32 *bitmap, uint32_t start, uint32_t len);
int xbitmap32_set(struct xbitmap32 *bitmap, uint32_t start, uint32_t len);
int xbitmap32_disunion(struct xbitmap32 *bitmap, struct xbitmap32 *sub);
uint32_t xbitmap32_hweight(struct xbitmap32 *bitmap);

/*
 * Return codes for the bitmap iterator functions are 0 to continue iterating,
 * and non-zero to stop iterating.  Any non-zero value will be passed up to the
 * iteration caller.  The special value -ECANCELED can be used to stop
 * iteration, because neither bitmap iterator ever generates that error code on
 * its own.  Callers must not modify the bitmap while walking it.
 */
typedef int (*xbitmap32_walk_fn)(uint32_t start, uint32_t len, void *priv);
int xbitmap32_walk(struct xbitmap32 *bitmap, xbitmap32_walk_fn fn,
		void *priv);

bool xbitmap32_empty(struct xbitmap32 *bitmap);
bool xbitmap32_test(struct xbitmap32 *bitmap, uint32_t start, uint32_t *len);

/* Bitmaps, but for type-checked for xfs_agblock_t */

struct xagb_bitmap {
	struct xbitmap32	agbitmap;
};

static inline void xagb_bitmap_init(struct xagb_bitmap *bitmap)
{
	xbitmap32_init(&bitmap->agbitmap);
}

static inline void xagb_bitmap_destroy(struct xagb_bitmap *bitmap)
{
	xbitmap32_destroy(&bitmap->agbitmap);
}

static inline int xagb_bitmap_clear(struct xagb_bitmap *bitmap,
		xfs_agblock_t start, xfs_extlen_t len)
{
	return xbitmap32_clear(&bitmap->agbitmap, start, len);
}
static inline int xagb_bitmap_set(struct xagb_bitmap *bitmap,
		xfs_agblock_t start, xfs_extlen_t len)
{
	return xbitmap32_set(&bitmap->agbitmap, start, len);
}

static inline bool xagb_bitmap_test(struct xagb_bitmap *bitmap,
		xfs_agblock_t start, xfs_extlen_t *len)
{
	return xbitmap32_test(&bitmap->agbitmap, start, len);
}

static inline int xagb_bitmap_disunion(struct xagb_bitmap *bitmap,
		struct xagb_bitmap *sub)
{
	return xbitmap32_disunion(&bitmap->agbitmap, &sub->agbitmap);
}

static inline uint32_t xagb_bitmap_hweight(struct xagb_bitmap *bitmap)
{
	return xbitmap32_hweight(&bitmap->agbitmap);
}
static inline bool xagb_bitmap_empty(struct xagb_bitmap *bitmap)
{
	return xbitmap32_empty(&bitmap->agbitmap);
}

static inline int xagb_bitmap_walk(struct xagb_bitmap *bitmap,
		xbitmap32_walk_fn fn, void *priv)
{
	return xbitmap32_walk(&bitmap->agbitmap, fn, priv);
}

int xagb_bitmap_set_btblocks(struct xagb_bitmap *bitmap,
		struct xfs_btree_cur *cur);
int xagb_bitmap_set_btcur_path(struct xagb_bitmap *bitmap,
		struct xfs_btree_cur *cur);

/* Bitmaps, but for type-checked for xfs_fsblock_t */

struct xfsb_bitmap {
	struct xbitmap	fsbitmap;
};

static inline void xfsb_bitmap_init(struct xfsb_bitmap *bitmap)
{
	xbitmap_init(&bitmap->fsbitmap);
}

static inline void xfsb_bitmap_destroy(struct xfsb_bitmap *bitmap)
{
	xbitmap_destroy(&bitmap->fsbitmap);
}

static inline int xfsb_bitmap_set(struct xfsb_bitmap *bitmap,
		xfs_fsblock_t start, xfs_filblks_t len)
{
	return xbitmap_set(&bitmap->fsbitmap, start, len);
}

static inline int xfsb_bitmap_walk(struct xfsb_bitmap *bitmap,
		xbitmap_walk_fn fn, void *priv)
{
	return xbitmap_walk(&bitmap->fsbitmap, fn, priv);
}

#endif	/* __XFS_SCRUB_BITMAP_H__ */
