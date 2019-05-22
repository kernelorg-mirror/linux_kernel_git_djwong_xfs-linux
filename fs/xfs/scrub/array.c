// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "scrub/array.h"

/*
 * XFS Fixed-Size Big Memory Array
 * ===============================
 * The big memory array uses a list to store large numbers of fixed-size
 * records in memory.  Access to the array is performed via indexed get and put
 * methods, and an append method is provided for convenience.  Array elements
 * can be set to all zeroes, which means that the entry is NULL and will be
 * skipped during iteration.
 */

struct xa_item {
	struct list_head	list;
	/* array item comes after here */
};

#define XA_ITEM_SIZE(sz)	(sizeof(struct xa_item) + (sz))

/* Initialize a big memory array. */
struct xfbma *
xfbma_init(
	size_t		obj_size)
{
	struct xfbma	*array;
	int		error;

	error = -ENOMEM;
	array = kmem_alloc(sizeof(struct xfbma) + obj_size,
			KM_NOFS | KM_MAYFAIL);
	if (!array)
		return ERR_PTR(error);

	array->obj_size = obj_size;
	array->nr = 0;
	INIT_LIST_HEAD(&array->list);
	memset(&array->cache, 0, sizeof(array->cache));

	return array;
}

void
xfbma_destroy(
	struct xfbma	*array)
{
	struct xa_item	*item, *n;

	list_for_each_entry_safe(item, n, &array->list, list) {
		list_del(&item->list);
		kmem_free(item);
	}
	kmem_free(array);
}

/* Find something in the cache. */
static struct xa_item *
xfbma_cache_lookup(
	struct xfbma	*array,
	uint64_t	nr)
{
	uint64_t	i;

	for (i = 0; i < XMA_CACHE_SIZE; i++)
		if (array->cache[i].nr == nr && array->cache[i].item)
			return array->cache[i].item;
	return NULL;
}

/* Invalidate the lookup cache. */
static void
xfbma_cache_invalidate(
	struct xfbma	*array)
{
	memset(array->cache, 0, sizeof(array->cache));
}

/* Put something in the cache. */
static void
xfbma_cache_store(
	struct xfbma	*array,
	uint64_t	nr,
	struct xa_item	*item)
{
	memmove(array->cache + 1, array->cache,
			sizeof(struct xma_cache) * (XMA_CACHE_SIZE - 1));
	array->cache[0].item = item;
	array->cache[0].nr = nr;
}

/* Find a particular array item. */
static struct xa_item *
xfbma_lookup(
	struct xfbma	*array,
	uint64_t	nr)
{
	struct xa_item	*item;
	uint64_t	i;

	if (nr >= array->nr) {
		ASSERT(0);
		return NULL;
	}

	item = xfbma_cache_lookup(array, nr);
	if (item)
		return item;

	i = 0;
	list_for_each_entry(item, &array->list, list) {
		if (i == nr) {
			xfbma_cache_store(array, nr, item);
			return item;
		}
		i++;
	}
	return NULL;
}

/* Get an element from the array. */
int
xfbma_get(
	struct xfbma	*array,
	uint64_t	nr,
	void		*ptr)
{
	struct xa_item	*item;

	item = xfbma_lookup(array, nr);
	if (!item)
		return -ENODATA;
	memcpy(ptr, item + 1, array->obj_size);
	return 0;
}

/* Put an element in the array. */
int
xfbma_set(
	struct xfbma	*array,
	uint64_t	nr,
	void		*ptr)
{
	struct xa_item	*item;

	item = xfbma_lookup(array, nr);
	if (!item)
		return -ENODATA;
	memcpy(item + 1, ptr, array->obj_size);
	return 0;
}

/* Is this array element NULL? */
bool
xfbma_is_null(
	struct xfbma	*array,
	void		*ptr)
{
	return !memchr_inv(ptr, 0, array->obj_size);
}

/* Put an element anywhere in the array that isn't NULL. */
int
xfbma_insert_anywhere(
	struct xfbma	*array,
	void		*ptr)
{
	struct xa_item	*item;

	/* Find a null slot to put it in. */
	list_for_each_entry(item, &array->list, list) {
		if (!xfbma_is_null(array, item + 1))
			continue;
		memcpy(item + 1, ptr, array->obj_size);
		return 0;
	}

	/* No null slots, just dump it on the end. */
	return xfbma_append(array, ptr);
}

/* NULL an element in the array. */
int
xfbma_nullify(
	struct xfbma	*array,
	uint64_t	nr)
{
	struct xa_item	*item;

	item = xfbma_lookup(array, nr);
	if (!item)
		return -ENODATA;
	memset(item + 1, 0, array->obj_size);
	return 0;
}

/* Append an element to the array. */
int
xfbma_append(
	struct xfbma	*array,
	void		*ptr)
{
	struct xa_item	*item;

	item = kmem_alloc(XA_ITEM_SIZE(array->obj_size), KM_NOFS | KM_MAYFAIL);
	if (!item)
		return -ENOMEM;

	INIT_LIST_HEAD(&item->list);
	memcpy(item + 1, ptr, array->obj_size);
	list_add_tail(&item->list, &array->list);
	array->nr++;
	return 0;
}

/*
 * Iterate every element in this array, freeing each element as we go.
 * Array elements will be shifted down.
 */
int
xfbma_iter_del(
	struct xfbma	*array,
	xfbma_iter_fn	iter_fn,
	void		*priv)
{
	struct xa_item	*item, *n;
	int		error = 0;

	list_for_each_entry_safe(item, n, &array->list, list) {
		if (xfbma_is_null(array, item + 1))
			goto next;
		memcpy(array + 1, item + 1, array->obj_size);
		error = iter_fn(array + 1, priv);
		if (error)
			break;
next:
		list_del(&item->list);
		kmem_free(item);
		array->nr--;
	}

	xfbma_cache_invalidate(array);
	return error;
}

/* Return length of array. */
uint64_t
xfbma_length(
	struct xfbma	*array)
{
	return array->nr;
}

static int
xfbma_item_cmp(
	void			*priv,
	struct list_head	*a,
	struct list_head	*b)
{
	int			(*cmp_fn)(void *a, void *b) = priv;
	struct xa_item		*ai, *bi;

	ai = container_of(a, struct xa_item, list);
	bi = container_of(b, struct xa_item, list);

	return cmp_fn(ai + 1, bi + 1);
}

/* Sort everything in this array. */
int
xfbma_sort(
	struct xfbma	*array,
	xfbma_cmp_fn	cmp_fn)
{
	list_sort(cmp_fn, &array->list, xfbma_item_cmp);
	return 0;
}
