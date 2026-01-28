// SPDX-License-Identifier: GPL-2.0
/*
 * The fuse_iext code comes from xfs_iext_tree.[ch] and is:
 * Copyright (c) 2017 Christoph Hellwig.
 *
 * Everything else is:
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef _FS_FUSE_IOMAP_CACHE_H
#define _FS_FUSE_IOMAP_CACHE_H

#if IS_ENABLED(CONFIG_FUSE_IOMAP)
/*
 * File incore extent information, present for read and write mappings.
 */
struct fuse_iext_root {
	/* bytes in ir_data, or -1 if it has never been used */
	int64_t			ir_bytes;
	void			*ir_data;	/* extent tree root */
	unsigned int		ir_height;	/* height of the extent tree */
};

struct fuse_iomap_cache {
	struct fuse_iext_root	ic_read;
	struct fuse_iext_root	ic_write;
	uint64_t		ic_seq;		/* validity counter */
	struct rw_semaphore	ic_lock;	/* mapping lock */
	struct inode		*ic_inode;
};

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

bool fuse_iext_get_extent(const struct fuse_iext_root *ir,
			  const struct fuse_iext_cursor *cur,
			  struct fuse_iomap_io *gotp);

static inline uint64_t fuse_iext_read_seq(struct fuse_iomap_cache *ic)
{
	return (uint64_t)READ_ONCE(ic->ic_seq);
}

static inline void fuse_iomap_cache_init(struct fuse_inode *fi)
{
	fi->cache = NULL;
}

static inline bool fuse_inode_caches_iomaps(const struct inode *inode)
{
	const struct fuse_inode *fi = get_fuse_inode(inode);

	return fi->cache != NULL;
}

int fuse_iomap_cache_alloc(struct inode *inode);
void fuse_iomap_cache_free(struct inode *inode);

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
#endif /* CONFIG_FUSE_IOMAP */

#endif /* _FS_FUSE_IOMAP_CACHE_H */
