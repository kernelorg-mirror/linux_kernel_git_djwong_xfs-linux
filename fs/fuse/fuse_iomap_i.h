// SPDX-License-Identifier: GPL-2.0
/*
 * The fuse_iext code comes from xfs_iext_tree.[ch] and is:
 * Copyright (c) 2017 Christoph Hellwig.
 *
 * Everything else is:
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef _FS_FUSE_IOMAP_I_H
#define _FS_FUSE_IOMAP_I_H

#if IS_ENABLED(CONFIG_FUSE_IOMAP)
#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG)

#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG_BY_DEFAULT)
DECLARE_STATIC_KEY_TRUE(fuse_iomap_debug);
#else
DECLARE_STATIC_KEY_FALSE(fuse_iomap_debug);
#endif /* FUSE_IOMAP_DEBUG_BY_DEFAULT */

# define ASSERT(condition) \
while (static_branch_unlikely(&fuse_iomap_debug)) {			\
	int __cond = !!(condition);					\
	if (unlikely(!__cond))						\
		trace_fuse_iomap_assert(__func__, __LINE__, #condition); \
	WARN(!__cond, "Assertion failed: %s, func: %s, line: %d", #condition, __func__, __LINE__); \
	break;								\
}
# define BAD_DATA(condition) ({						\
	int __cond = !!(condition);					\
	if (unlikely(__cond))						\
		trace_fuse_iomap_bad_data(__func__, __LINE__, #condition); \
	if (static_branch_unlikely(&fuse_iomap_debug))			\
		WARN(__cond, "Bad mapping: %s, func: %s, line: %d", #condition, __func__, __LINE__); \
	unlikely(__cond);								\
})
#else
# define ASSERT(condition)
# define BAD_DATA(condition) ({						\
	int __cond = !!(condition);					\
	if (unlikely(__cond))						\
		trace_fuse_iomap_bad_data(__func__, __LINE__, #condition); \
	unlikely(__cond);						\
})
#endif /* CONFIG_FUSE_IOMAP_DEBUG */

void fuse_iomap_cache_lock(struct inode *inode);
void fuse_iomap_cache_unlock(struct inode *inode);
void fuse_iomap_cache_lock_shared(struct inode *inode);
void fuse_iomap_cache_unlock_shared(struct inode *inode);

struct fuse_iext_leaf;

struct fuse_iext_cursor {
	struct fuse_iext_leaf	*leaf;
	int			pos;
};

#define FUSE_IEXT_LEFT_CONTIG	(1u << 0)
#define FUSE_IEXT_RIGHT_CONTIG	(1u << 1)
#define FUSE_IEXT_LEFT_FILLING	(1u << 2)
#define FUSE_IEXT_RIGHT_FILLING	(1u << 3)
#define FUSE_IEXT_LEFT_VALID	(1u << 4)
#define FUSE_IEXT_RIGHT_VALID	(1u << 5)
#define FUSE_IEXT_WRITE_MAPPING	(1u << 6)

struct fuse_ifork *fuse_iext_state_to_fork(struct fuse_iomap_cache *ip,
		unsigned int state);

uint64_t	fuse_iext_count(const struct fuse_ifork *ifp);
void		fuse_iext_insert_raw(struct fuse_iomap_cache *ip,
			struct fuse_ifork *ifp,
			struct fuse_iext_cursor *cur,
			const struct fuse_iomap_io *irec);
void		fuse_iext_insert(struct fuse_iomap_cache *,
			struct fuse_iext_cursor *cur,
			const struct fuse_iomap_io *, int);
void		fuse_iext_remove(struct fuse_iomap_cache *,
			struct fuse_iext_cursor *,
			int);
void		fuse_iext_destroy(struct fuse_ifork *);

bool		fuse_iext_lookup_extent(struct fuse_iomap_cache *ip,
			struct fuse_ifork *ifp, loff_t bno,
			struct fuse_iext_cursor *cur,
			struct fuse_iomap_io *gotp);
bool		fuse_iext_lookup_extent_before(struct fuse_iomap_cache *ip,
			struct fuse_ifork *ifp, loff_t *end,
			struct fuse_iext_cursor *cur,
			struct fuse_iomap_io *gotp);
bool		fuse_iext_get_extent(const struct fuse_ifork *ifp,
			const struct fuse_iext_cursor *cur,
			struct fuse_iomap_io *gotp);
void		fuse_iext_update_extent(struct fuse_iomap_cache *ip, int state,
			struct fuse_iext_cursor *cur,
			struct fuse_iomap_io *gotp);

void		fuse_iext_first(struct fuse_ifork *, struct fuse_iext_cursor *);
void		fuse_iext_last(struct fuse_ifork *, struct fuse_iext_cursor *);
void		fuse_iext_next(struct fuse_ifork *, struct fuse_iext_cursor *);
void		fuse_iext_prev(struct fuse_ifork *, struct fuse_iext_cursor *);

static inline bool fuse_iext_next_extent(struct fuse_ifork *ifp,
		struct fuse_iext_cursor *cur, struct fuse_iomap_io *gotp)
{
	fuse_iext_next(ifp, cur);
	return fuse_iext_get_extent(ifp, cur, gotp);
}

static inline bool fuse_iext_prev_extent(struct fuse_ifork *ifp,
		struct fuse_iext_cursor *cur, struct fuse_iomap_io *gotp)
{
	fuse_iext_prev(ifp, cur);
	return fuse_iext_get_extent(ifp, cur, gotp);
}

/*
 * Return the extent after cur in gotp without updating the cursor.
 */
static inline bool fuse_iext_peek_next_extent(struct fuse_ifork *ifp,
		struct fuse_iext_cursor *cur, struct fuse_iomap_io *gotp)
{
	struct fuse_iext_cursor ncur = *cur;

	fuse_iext_next(ifp, &ncur);
	return fuse_iext_get_extent(ifp, &ncur, gotp);
}

/*
 * Return the extent before cur in gotp without updating the cursor.
 */
static inline bool fuse_iext_peek_prev_extent(struct fuse_ifork *ifp,
		struct fuse_iext_cursor *cur, struct fuse_iomap_io *gotp)
{
	struct fuse_iext_cursor ncur = *cur;

	fuse_iext_prev(ifp, &ncur);
	return fuse_iext_get_extent(ifp, &ncur, gotp);
}

#define for_each_fuse_iext(ifp, ext, got)		\
	for (fuse_iext_first((ifp), (ext));		\
	     fuse_iext_get_extent((ifp), (ext), (got));	\
	     fuse_iext_next((ifp), (ext)))

/* iomaps that come direct from the fuse server are presumed to be valid */
#define FUSE_IOMAP_ALWAYS_VALID	((uint64_t)0)
/* set initial iomap cookie value to avoid ALWAYS_VALID */
#define FUSE_IOMAP_INIT_COOKIE	((uint64_t)1)

static inline uint64_t fuse_iext_read_seq(struct fuse_iomap_cache *ip)
{
	return (uint64_t)READ_ONCE(ip->im_seq);
}

int fuse_iomap_cache_remove(struct inode *inode, enum fuse_iomap_iodir iodir,
			    loff_t off, uint64_t len);

int fuse_iomap_cache_upsert(struct inode *inode, enum fuse_iomap_iodir iodir,
			    const struct fuse_iomap_io *map);

enum fuse_iomap_lookup_result {
	LOOKUP_HIT,
	LOOKUP_MISS,
	LOOKUP_NOFORK,
};

struct fuse_iomap_lookup {
	struct fuse_iomap_io	map;		 /* cached mapping */
	uint64_t		validity_cookie; /* used with .iomap_valid() */
};

enum fuse_iomap_lookup_result
fuse_iomap_cache_lookup(struct inode *inode, enum fuse_iomap_iodir iodir,
			loff_t off, uint64_t len,
			struct fuse_iomap_lookup *mval);

int fuse_iomap_cache_invalidate_range(struct inode *inode, loff_t offset,
				      uint64_t length);
static inline int fuse_iomap_cache_invalidate(struct inode *inode,
					      loff_t offset)
{
	return fuse_iomap_cache_invalidate_range(inode, offset,
						 FUSE_IOMAP_INVAL_TO_EOF);
}

/* absolute maximum memory consumption per iomap mapping cache */
#define FUSE_IOMAP_CACHE_MAX_MAXBYTES		(SZ_2M)

/* default maximum memory consumption per iomap mapping cache */
#define FUSE_IOMAP_CACHE_DEFAULT_MAXBYTES	(SZ_256K)

void fuse_iomap_cache_set_maxbytes(struct fuse_conn *fc, unsigned int maxbytes);

#endif /* CONFIG_FUSE_IOMAP */

#endif /* _FS_FUSE_IOMAP_I_H */
