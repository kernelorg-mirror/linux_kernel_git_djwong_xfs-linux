// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "scrub/array.h"
#include "scrub/blob.h"
#include "scrub/xfile.h"

/*
 * XFS Blob Storage
 * ================
 * Stores and retrieves blobs using a memfd object.  Objects are appended to
 * the file and the offset is returned as a magic cookie for retrieval.
 */

#define XB_KEY_MAGIC	0xABAADDAD
struct xb_key {
	uint32_t		magic;
	uint32_t		size;
	loff_t			offset;
	/* blob comes after here */
} __packed;

/* Initialize a blob storage object. */
struct xblob *
xblob_init(void)
{
	struct xblob	*blob;
	struct file	*filp;
	int		error;

	filp = xfile_create("blob storage");
	if (!filp)
		return ERR_PTR(-ENOMEM);
	if (IS_ERR(filp))
		return ERR_CAST(filp);

	error = -ENOMEM;
	blob = kmem_alloc(sizeof(struct xblob), KM_NOFS | KM_MAYFAIL);
	if (!blob)
		goto out_filp;

	blob->filp = filp;
	blob->last_offset = PAGE_SIZE;
	blob->io_flags = 0;
	return blob;
out_filp:
	fput(filp);
	return ERR_PTR(error);
}

/* Destroy a blob storage object. */
void
xblob_destroy(
	struct xblob	*blob)
{
	xfile_destroy(blob->filp);
	kmem_free(blob);
}

/* Retrieve a blob. */
int
xblob_get(
	struct xblob	*blob,
	xblob_cookie	cookie,
	void		*ptr,
	uint32_t	size)
{
	struct xb_key	key;
	loff_t		pos = cookie;
	int		error;

	error = xfile_io(blob->filp, blob->io_flags | XFILE_IO_READ, &pos,
			&key, sizeof(key));
	if (error)
		return error;

	if (key.magic != XB_KEY_MAGIC || key.offset != cookie) {
		ASSERT(0);
		return -ENODATA;
	}
	if (size < key.size) {
		ASSERT(0);
		return -EFBIG;
	}

	return xfile_io(blob->filp, blob->io_flags | XFILE_IO_READ, &pos, ptr,
			key.size);
}

/* Store a blob. */
int
xblob_put(
	struct xblob	*blob,
	xblob_cookie	*cookie,
	void		*ptr,
	uint32_t	size)
{
	struct xb_key	key = {
		.offset = blob->last_offset,
		.magic = XB_KEY_MAGIC,
		.size = size,
	};
	loff_t		pos = blob->last_offset;
	int		error;

	error = xfile_io(blob->filp, blob->io_flags | XFILE_IO_WRITE, &pos,
			&key, sizeof(key));
	if (error)
		goto out_err;

	error = xfile_io(blob->filp, blob->io_flags | XFILE_IO_WRITE, &pos,
			ptr, size);
	if (error)
		goto out_err;

	*cookie = blob->last_offset;
	blob->last_offset = pos;
	return 0;
out_err:
	xfile_discard(blob->filp, blob->last_offset, pos - 1);
	return -ENOMEM;
}

/* Free a blob. */
int
xblob_free(
	struct xblob	*blob,
	xblob_cookie	cookie)
{
	struct xb_key	key;
	loff_t		pos = cookie;
	int		error;

	error = xfile_io(blob->filp, blob->io_flags | XFILE_IO_READ, &pos,
			&key, sizeof(key));
	if (error)
		return error;

	if (key.magic != XB_KEY_MAGIC || key.offset != cookie) {
		ASSERT(0);
		return -ENODATA;
	}

	xfile_discard(blob->filp, cookie, cookie + sizeof(key) + key.size - 1);
	return 0;
}
