// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_BITMAP_H__
#define __XFS_SCRUB_BITMAP_H__

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
uint64_t xbitmap_count_set_regions(struct xbitmap *bitmap);

int xbitmap_take_first_set(struct xbitmap *bitmap, uint64_t start,
		uint64_t last, uint64_t *valp);

/* Bitmaps, but for type-checked for xfs_agblock_t */

struct xagb_bitmap {
	struct xbitmap	agbitmap;
};

static inline void xagb_bitmap_init(struct xagb_bitmap *bitmap)
{
	xbitmap_init(&bitmap->agbitmap);
}

static inline void xagb_bitmap_destroy(struct xagb_bitmap *bitmap)
{
	xbitmap_destroy(&bitmap->agbitmap);
}

static inline int xagb_bitmap_clear(struct xagb_bitmap *bitmap,
		xfs_agblock_t start, xfs_extlen_t len)
{
	return xbitmap_clear(&bitmap->agbitmap, start, len);
}
static inline int xagb_bitmap_set(struct xagb_bitmap *bitmap,
		xfs_agblock_t start, xfs_extlen_t len)
{
	return xbitmap_set(&bitmap->agbitmap, start, len);
}

static inline bool
xagb_bitmap_test(
	struct xagb_bitmap	*bitmap,
	xfs_agblock_t		start,
	xfs_extlen_t		*len)
{
	uint64_t		biglen = *len;
	bool			ret;

	ret = xbitmap_test(&bitmap->agbitmap, start, &biglen);

	if (start + biglen >= UINT_MAX) {
		ASSERT(0);
		biglen = UINT_MAX - start;
	}

	*len = biglen;
	return ret;
}

static inline int xagb_bitmap_disunion(struct xagb_bitmap *bitmap,
		struct xagb_bitmap *sub)
{
	return xbitmap_disunion(&bitmap->agbitmap, &sub->agbitmap);
}

static inline uint32_t xagb_bitmap_count_set_regions(struct xagb_bitmap *bitmap)
{
	return xbitmap_count_set_regions(&bitmap->agbitmap);
}
static inline uint32_t xagb_bitmap_hweight(struct xagb_bitmap *bitmap)
{
	return xbitmap_hweight(&bitmap->agbitmap);
}
static inline bool xagb_bitmap_empty(struct xagb_bitmap *bitmap)
{
	return xbitmap_empty(&bitmap->agbitmap);
}

static inline int xagb_bitmap_walk(struct xagb_bitmap *bitmap,
		xbitmap_walk_fn fn, void *priv)
{
	return xbitmap_walk(&bitmap->agbitmap, fn, priv);
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

/* Bitmaps, but for type-checked for xfs_fileoff_t */

struct xfo_bitmap {
	struct xbitmap	fobitmap;
};

static inline void xfo_bitmap_init(struct xfo_bitmap *bitmap)
{
	xbitmap_init(&bitmap->fobitmap);
}

static inline void xfo_bitmap_destroy(struct xfo_bitmap *bitmap)
{
	xbitmap_destroy(&bitmap->fobitmap);
}

static inline int xfo_bitmap_set(struct xfo_bitmap *bitmap,
		xfs_fileoff_t off, xfs_filblks_t len)
{
	return xbitmap_set(&bitmap->fobitmap, off, len);
}

static inline int xfo_bitmap_walk(struct xfo_bitmap *bitmap,
		xbitmap_walk_fn fn, void *priv)
{
	return xbitmap_walk(&bitmap->fobitmap, fn, priv);
}

#endif	/* __XFS_SCRUB_BITMAP_H__ */
