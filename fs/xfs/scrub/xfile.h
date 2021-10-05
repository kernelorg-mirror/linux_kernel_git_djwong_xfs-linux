/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_XFILE_H__
#define __XFS_SCRUB_XFILE_H__

#if IS_ENABLED(CONFIG_XFS_ONLINE_SCRUB)
struct xfile {
	struct file		*file;
};

struct xfile *xfile_create(const char *description, loff_t size);
void xfile_destroy(struct xfile *xf);

ssize_t xfile_pread(struct xfile *xf, void *buf, size_t count, loff_t pos);
ssize_t xfile_pwrite(struct xfile *xf, void *buf, size_t count, loff_t pos);

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
xfile_obj_store(struct xfile *xf, void *buf, size_t count, loff_t pos)
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

int xfile_dump(struct xfile *xf);
int xfile_obj_get_page(struct xfile *xf, loff_t offset, unsigned int len,
		struct page **pagep);
int xfile_obj_put_page(struct xfile *xf, loff_t offset, unsigned int len,
		struct page *page);
#else
static inline int
xfile_obj_load(struct xfile *xf, void *buf, size_t count, loff_t offset)
{
	return -EIO;
}

static inline int
xfile_obj_store(struct xfile *xf, void *buf, size_t count, loff_t offset)
{
	return -EIO;
}
static inline int
xfile_obj_get_page(
	struct xfile	*xf,
	loff_t		offset,
	unsigned int	len,
	struct page	**pagep)
{
	return -EIO;
}
static inline int
xfile_obj_put_page(
	struct xfile	*xf,
	loff_t		offset,
	unsigned int	len,
	struct page	*page)
{
	return -EIO;
}
#endif /* CONFIG_XFS_ONLINE_SCRUB */

#endif /* __XFS_SCRUB_XFILE_H__ */
