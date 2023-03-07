/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2018-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_XFILE_H__
#define __XFS_SCRUB_XFILE_H__

#ifdef CONFIG_XFS_IN_MEMORY_FILE

struct xfile_page {
	struct page		*page;
	void			*fsdata;
	loff_t			pos;
};

static inline bool xfile_page_cached(const struct xfile_page *xfpage)
{
	return xfpage->page != NULL;
}

static inline pgoff_t xfile_page_index(const struct xfile_page *xfpage)
{
	return xfpage->page->index;
}

struct xfile {
	struct file		*file;
	struct xfs_buf_cache	bcache;
};

int xfile_create(const char *description, loff_t isize, struct xfile **xfilep);
void xfile_destroy(struct xfile *xf);

ssize_t xfile_pread(struct xfile *xf, void *buf, size_t count, loff_t pos);
ssize_t xfile_pwrite(struct xfile *xf, const void *buf, size_t count,
		loff_t pos);

/*
 * Load an object.  Since we're treating this file as "memory", any error or
 * short IO is treated as a failure to allocate memory.
 */
static inline int
xfile_obj_load(struct xfile *xf, void *buf, size_t count, loff_t pos)
{
	ssize_t	ret = xfile_pread(xf, buf, count, pos);

	if (ret < 0 || ret != count)
		return -ENOMEM;
	return 0;
}

/*
 * Store an object.  Since we're treating this file as "memory", any error or
 * short IO is treated as a failure to allocate memory.
 */
static inline int
xfile_obj_store(struct xfile *xf, const void *buf, size_t count, loff_t pos)
{
	ssize_t	ret = xfile_pwrite(xf, buf, count, pos);

	if (ret < 0 || ret != count)
		return -ENOMEM;
	return 0;
}

void xfile_discard(struct xfile *xf, loff_t pos, u64 count);
int xfile_prealloc(struct xfile *xf, loff_t pos, u64 count);
loff_t xfile_seek_data(struct xfile *xf, loff_t pos);

struct xfile_stat {
	loff_t			size;
	unsigned long long	bytes;
};

int xfile_stat(struct xfile *xf, struct xfile_stat *statbuf);

int xfile_get_page(struct xfile *xf, loff_t offset, unsigned int len,
		struct xfile_page *xbuf);
int xfile_put_page(struct xfile *xf, struct xfile_page *xbuf);

int xfile_dump(struct xfile *xf);

static inline loff_t xfile_size(struct xfile *xf)
{
	return i_size_read(file_inode(xf->file));
}

static inline unsigned long long xfile_bytes(struct xfile *xf)
{
	struct xfile_stat	xs;
	int			ret;

	ret = xfile_stat(xf, &xs);
	if (ret)
		return 0;

	return xs.bytes;
}

/* file block (aka system page size) to basic block conversions. */
typedef unsigned long long	xfileoff_t;
#define XFB_BLOCKSIZE		(PAGE_SIZE)
#define XFB_BSHIFT		(PAGE_SHIFT)
#define XFB_SHIFT		(XFB_BSHIFT - BBSHIFT)

static inline loff_t xfo_to_b(xfileoff_t xfoff)
{
	return xfoff << XFB_BSHIFT;
}

static inline xfileoff_t b_to_xfo(loff_t pos)
{
	return (pos + (XFB_BLOCKSIZE - 1)) >> XFB_BSHIFT;
}

static inline xfileoff_t b_to_xfot(loff_t pos)
{
	return pos >> XFB_BSHIFT;
}

static inline xfs_daddr_t xfo_to_daddr(xfileoff_t xfoff)
{
	return xfoff << XFB_SHIFT;
}

static inline xfileoff_t xfs_daddr_to_xfo(xfs_daddr_t bb)
{
	return (bb + (xfo_to_daddr(1) - 1)) >> XFB_SHIFT;
}

static inline xfileoff_t xfs_daddr_to_xfot(xfs_daddr_t bb)
{
	return bb >> XFB_SHIFT;
}
#else
static inline int
xfile_obj_load(struct xfile *xf, void *buf, size_t count, loff_t offset)
{
	return -EIO;
}

static inline int
xfile_obj_store(struct xfile *xf, const void *buf, size_t count, loff_t offset)
{
	return -EIO;
}

static inline loff_t xfile_size(struct xfile *xf)
{
	return 0;
}
#endif /* CONFIG_XFS_IN_MEMORY_FILE */

#endif /* __XFS_SCRUB_XFILE_H__ */
