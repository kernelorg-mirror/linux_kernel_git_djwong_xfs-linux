// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "scrub/xfarray.h"
#include "scrub/xfblob.h"
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
int
xfblob_create(
	struct xfs_mount	*mp,
	const char		*description,
	struct xfblob		**blobp)
{
	struct xfblob		*blob;
	struct xfile		*xfile;
	int			error;

	error = xfile_create(mp, description, 0, &xfile);
	if (error)
		return error;

	error = -ENOMEM;
	blob = kmem_alloc(sizeof(struct xfblob), KM_NOFS | KM_MAYFAIL);
	if (!blob)
		goto out_xfile;

	blob->xfile = xfile;
	blob->last_offset = PAGE_SIZE;

	*blobp = blob;
	return 0;

out_xfile:
	xfile_destroy(xfile);
	return error;
}

/* Destroy a blob storage object. */
void
xfblob_destroy(
	struct xfblob	*blob)
{
	xfile_destroy(blob->xfile);
	kmem_free(blob);
}

/* Retrieve a blob. */
int
xfblob_load(
	struct xfblob	*blob,
	xfblob_cookie	cookie,
	void		*ptr,
	uint32_t	size)
{
	struct xb_key	key;
	int		error;

	error = xfile_obj_load(blob->xfile, &key, sizeof(key), cookie);
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

	return xfile_obj_load(blob->xfile, ptr, key.size, cookie + sizeof(key));
}

/* Store a blob. */
int
xfblob_store(
	struct xfblob	*blob,
	xfblob_cookie	*cookie,
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

	error = xfile_obj_store(blob->xfile, &key, sizeof(key), pos);
	if (error)
		return error;

	pos += sizeof(key);
	error = xfile_obj_store(blob->xfile, ptr, size, pos);
	if (error)
		goto out_err;

	*cookie = blob->last_offset;
	blob->last_offset += sizeof(key) + size;
	return 0;
out_err:
	xfile_discard(blob->xfile, blob->last_offset, sizeof(key));
	return error;
}

/* Free a blob. */
int
xfblob_free(
	struct xfblob	*blob,
	xfblob_cookie	cookie)
{
	struct xb_key	key;
	int		error;

	error = xfile_obj_load(blob->xfile, &key, sizeof(key), cookie);
	if (error)
		return error;

	if (key.magic != XB_KEY_MAGIC || key.offset != cookie) {
		ASSERT(0);
		return -ENODATA;
	}

	xfile_discard(blob->xfile, cookie, sizeof(key) + key.size);
	return 0;
}
