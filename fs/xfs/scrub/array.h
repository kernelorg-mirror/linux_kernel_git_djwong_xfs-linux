/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SCRUB_ARRAY_H__
#define __XFS_SCRUB_ARRAY_H__

struct xfbma {
	/* Underlying file that backs the array. */
	struct xfile	*xfile;

	/* Number of array elements. */
	uint64_t	nr;

	/* Maximum possible array size. */
	uint64_t	max_nr;

	/* Size of an array element. */
	size_t		obj_size;
};

struct xfbma *xfbma_init(const char *descr, size_t obj_size);
void xfbma_destroy(struct xfbma *array);
int xfbma_get(struct xfbma *array, uint64_t idx, void *ptr);
int xfbma_set(struct xfbma *array, uint64_t idx, void *ptr);
int xfbma_insert_anywhere(struct xfbma *array, void *ptr);
bool xfbma_is_null(struct xfbma *array, void *ptr);
int xfbma_nullify(struct xfbma *array, uint64_t idx);

/* Append an element to the array. */
static inline int xfbma_append(struct xfbma *array, void *ptr)
{
	return xfbma_set(array, array->nr, ptr);
}

uint64_t xfbma_length(struct xfbma *array);
int xfbma_iter_get(struct xfbma *array, uint64_t *idx, void *rec);

/*
 * Return codes for the array iterator function are 0 to continue iterating,
 * and non-zero to stop iterating.  Any non-zero value will be passed up to the
 * iteration caller.  The special value -ECANCELED can be used to stop
 * iteration, because the iterator never generates that error code on its own.
 */
typedef int (*xfbma_iter_fn)(const void *item, void *priv);

int xfbma_iter_del(struct xfbma *array, xfbma_iter_fn iter_fn, void *priv);

typedef int (*xfbma_cmp_fn)(const void *a, const void *b);

int xfbma_sort(struct xfbma *array, xfbma_cmp_fn cmp_fn);

#define foreach_xfbma_item(array, i, rec) \
	for ((i) = 0; (i) < xfbma_length((array)); (i)++) \
		if (xfbma_get((array), (i), &(rec)) == 0 && \
		    !xfbma_is_null((array), &(rec)))

#endif /* __XFS_SCRUB_ARRAY_H__ */
