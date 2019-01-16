// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SCRUB_BLOB_H__
#define __XFS_SCRUB_BLOB_H__

struct xblob {
	struct file	*filp;
	loff_t		last_offset;
};

typedef loff_t		xblob_cookie;

struct xblob *xblob_init(void);
void xblob_destroy(struct xblob *blob);
int xblob_get(struct xblob *blob, xblob_cookie cookie, void *ptr,
		uint32_t size);
int xblob_put(struct xblob *blob, xblob_cookie *cookie, void *ptr,
		uint32_t size);
int xblob_free(struct xblob *blob, xblob_cookie cookie);

#endif /* __XFS_SCRUB_BLOB_H__ */
