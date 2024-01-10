/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2018-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_XFILE_H__
#define __XFS_SCRUB_XFILE_H__

struct xfile {
	struct file		*file;
};

int xfile_create(const char *description, loff_t isize, struct xfile **xfilep);
void xfile_destroy(struct xfile *xf);

int xfile_load(struct xfile *xf, void *buf, size_t count, loff_t pos);
int xfile_store(struct xfile *xf, const void *buf, size_t count,
		loff_t pos);

loff_t xfile_seek_data(struct xfile *xf, loff_t pos);

#define XFILE_ALLOC		(1 << 0) /* allocate page if not present */
struct page *xfile_get_page(struct xfile *xf, loff_t offset, unsigned int len,
		unsigned int flags);
void xfile_put_page(struct xfile *xf, struct page *page);

#endif /* __XFS_SCRUB_XFILE_H__ */
