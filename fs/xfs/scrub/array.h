// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SCRUB_ARRAY_H__
#define __XFS_SCRUB_ARRAY_H__

struct xma_item;

struct xma_cache {
	uint64_t	nr;
	struct xa_item	*item;
};

#define XMA_CACHE_SIZE	(8)

struct xfbma {
	struct list_head	list;
	size_t			obj_size;
	uint64_t		nr;
	struct xma_cache	cache[XMA_CACHE_SIZE];
};

struct xfbma *xfbma_init(size_t obj_size);
void xfbma_destroy(struct xfbma *array);
int xfbma_get(struct xfbma *array, uint64_t nr, void *ptr);
int xfbma_set(struct xfbma *array, uint64_t nr, void *ptr);
int xfbma_insert_anywhere(struct xfbma *array, void *ptr);
bool xfbma_is_null(struct xfbma *array, void *ptr);
int xfbma_nullify(struct xfbma *array, uint64_t nr);
int xfbma_append(struct xfbma *array, void *ptr);
uint64_t xfbma_length(struct xfbma *array);

/*
 * Iterator functions return zero for success, a negative error code to abort
 * with an error, or XFBMA_ITERATE_ABORT to stop iterating.
 */
#define XFBMA_ITERATE_ABORT	(1)
typedef int (*xfbma_iter_fn)(const void *item, void *priv);

int xfbma_iter_del(struct xfbma *array, xfbma_iter_fn iter_fn, void *priv);

typedef int (*xfbma_cmp_fn)(const void *a, const void *b);

int xfbma_sort(struct xfbma *array, xfbma_cmp_fn cmp_fn);

#define foreach_xfbma_item(array, i, rec) \
	for ((i) = 0; (i) < xfbma_length(array); (i)++) \
		if (xfbma_get((array), (i), &(rec)) == 0 && \
		    !xfbma_is_null((array), &(rec)))

#endif /* __XFS_SCRUB_ARRAY_H__ */
