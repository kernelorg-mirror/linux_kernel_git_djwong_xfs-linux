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

/*
 * XFS Blob Storage
 * ================
 * Stores and retrieves blobs using a list.  Objects are appended to
 * the list and the pointer is returned as a magic cookie for retrieval.
 */

#define XB_KEY_MAGIC	0xABAADDAD
struct xb_key {
	struct list_head	list;
	uint32_t		magic;
	uint32_t		size;
	/* blob comes after here */
} __packed;

#define XB_KEY_SIZE(sz)	(sizeof(struct xb_key) + (sz))

/* Initialize a blob storage object. */
struct xblob *
xblob_init(void)
{
	struct xblob	*blob;
	int		error;

	error = -ENOMEM;
	blob = kmem_alloc(sizeof(struct xblob), KM_NOFS | KM_MAYFAIL);
	if (!blob)
		return ERR_PTR(error);

	INIT_LIST_HEAD(&blob->list);
	return blob;
}

/* Destroy a blob storage object. */
void
xblob_destroy(
	struct xblob	*blob)
{
	struct xb_key	*key, *n;

	list_for_each_entry_safe(key, n, &blob->list, list) {
		list_del(&key->list);
		kmem_free(key);
	}
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
	struct xb_key	*key = (struct xb_key *)cookie;

	if (key->magic != XB_KEY_MAGIC) {
		ASSERT(0);
		return -ENODATA;
	}
	if (size < key->size) {
		ASSERT(0);
		return -EFBIG;
	}

	memcpy(ptr, key + 1, key->size);
	return 0;
}

/* Store a blob. */
int
xblob_put(
	struct xblob	*blob,
	xblob_cookie	*cookie,
	void		*ptr,
	uint32_t	size)
{
	struct xb_key	*key;

	key = kmem_alloc(XB_KEY_SIZE(size), KM_NOFS | KM_MAYFAIL);
	if (!key)
		return -ENOMEM;

	INIT_LIST_HEAD(&key->list);
	list_add_tail(&key->list, &blob->list);
	key->magic = XB_KEY_MAGIC;
	key->size = size;
	memcpy(key + 1, ptr, size);
	*cookie = (xblob_cookie)key;
	return 0;
}

/* Free a blob. */
int
xblob_free(
	struct xblob	*blob,
	xblob_cookie	cookie)
{
	struct xb_key	*key = (struct xb_key *)cookie;

	if (key->magic != XB_KEY_MAGIC) {
		ASSERT(0);
		return -ENODATA;
	}
	key->magic = 0;
	list_del(&key->list);
	kmem_free(key);
	return 0;
}
