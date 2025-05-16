// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2017 Christoph Hellwig.
 */

#ifndef _FS_FUSE_IOMAP_CACHE_H
#define _FS_FUSE_IOMAP_CACHE_H

#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG)
# define ASSERT(a)		do { WARN(!(a), "Assertion failed: %s, func: %s, line: %d", #a, __func__, __LINE__); } while (0)
# define BAD_DATA(condition)	(WARN(condition, "Bad mapping: %s, func: %s, line: %d", #condition, __func__, __LINE__))
#else
# define ASSERT(a)
# define BAD_DATA(condition)	(condition)
#endif

#define FUSE_IOMAP_LOCK_SHARED	(1U << 0)
#define FUSE_IOMAP_LOCK_EXCL	(1U << 1)

void fuse_iomap_cache_lock(struct inode *inode, unsigned int lock_flags);
void fuse_iomap_cache_unlock(struct inode *inode, unsigned int lock_flags);

#define FUSE_IOMAP_MAX_LEN	((loff_t)(1ULL << 63))

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
#define FUSE_IEXT_WRITEFORK	(1u << 6)

struct fuse_ifork *fuse_iext_state_to_fork(struct fuse_iomap_cache *ip,
		unsigned int state);

uint64_t	fuse_iext_count(const struct fuse_ifork *ifp);
void		fuse_iext_insert_raw(struct fuse_iomap_cache *ip,
			struct fuse_ifork *ifp,
			struct fuse_iext_cursor *cur,
			const struct fuse_iomap *irec);
void		fuse_iext_insert(struct fuse_iomap_cache *,
			struct fuse_iext_cursor *cur,
			const struct fuse_iomap *, int);
void		fuse_iext_remove(struct fuse_iomap_cache *,
			struct fuse_iext_cursor *,
			int);
void		fuse_iext_destroy(struct fuse_ifork *);

bool		fuse_iext_lookup_extent(struct fuse_iomap_cache *ip,
			struct fuse_ifork *ifp, loff_t bno,
			struct fuse_iext_cursor *cur,
			struct fuse_iomap *gotp);
bool		fuse_iext_lookup_extent_before(struct fuse_iomap_cache *ip,
			struct fuse_ifork *ifp, loff_t *end,
			struct fuse_iext_cursor *cur,
			struct fuse_iomap *gotp);
bool		fuse_iext_get_extent(const struct fuse_ifork *ifp,
			const struct fuse_iext_cursor *cur,
			struct fuse_iomap *gotp);
void		fuse_iext_update_extent(struct fuse_iomap_cache *ip, int state,
			struct fuse_iext_cursor *cur,
			struct fuse_iomap *gotp);

void		fuse_iext_first(struct fuse_ifork *, struct fuse_iext_cursor *);
void		fuse_iext_last(struct fuse_ifork *, struct fuse_iext_cursor *);
void		fuse_iext_next(struct fuse_ifork *, struct fuse_iext_cursor *);
void		fuse_iext_prev(struct fuse_ifork *, struct fuse_iext_cursor *);

static inline bool fuse_iext_next_extent(struct fuse_ifork *ifp,
		struct fuse_iext_cursor *cur, struct fuse_iomap *gotp)
{
	fuse_iext_next(ifp, cur);
	return fuse_iext_get_extent(ifp, cur, gotp);
}

static inline bool fuse_iext_prev_extent(struct fuse_ifork *ifp,
		struct fuse_iext_cursor *cur, struct fuse_iomap *gotp)
{
	fuse_iext_prev(ifp, cur);
	return fuse_iext_get_extent(ifp, cur, gotp);
}

/*
 * Return the extent after cur in gotp without updating the cursor.
 */
static inline bool fuse_iext_peek_next_extent(struct fuse_ifork *ifp,
		struct fuse_iext_cursor *cur, struct fuse_iomap *gotp)
{
	struct fuse_iext_cursor ncur = *cur;

	fuse_iext_next(ifp, &ncur);
	return fuse_iext_get_extent(ifp, &ncur, gotp);
}

/*
 * Return the extent before cur in gotp without updating the cursor.
 */
static inline bool fuse_iext_peek_prev_extent(struct fuse_ifork *ifp,
		struct fuse_iext_cursor *cur, struct fuse_iomap *gotp)
{
	struct fuse_iext_cursor ncur = *cur;

	fuse_iext_prev(ifp, &ncur);
	return fuse_iext_get_extent(ifp, &ncur, gotp);
}

#define for_each_fuse_iext(ifp, ext, got)		\
	for (fuse_iext_first((ifp), (ext));		\
	     fuse_iext_get_extent((ifp), (ext), (got));	\
	     fuse_iext_next((ifp), (ext)))

#endif /* _FS_FUSE_IOMAP_CACHE_H */
