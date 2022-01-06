/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_XFARRAY_H__
#define __XFS_SCRUB_XFARRAY_H__

struct xfarray {
	/* Underlying file that backs the array. */
	struct xfile	*xfile;

	/* Number of array elements. */
	uint64_t	nr;

	/* Maximum possible array size. */
	uint64_t	max_nr;

	/* Size of an array element. */
	size_t		obj_size;
};

int xfarray_create(struct xfs_mount *mp, const char *descr, size_t obj_size,
		struct xfarray **arrayp);
void xfarray_destroy(struct xfarray *array);
int xfarray_load(struct xfarray *array, uint64_t idx, void *ptr);
int xfarray_store(struct xfarray *array, uint64_t idx, void *ptr);
int xfarray_store_anywhere(struct xfarray *array, void *ptr);
bool xfarray_is_null(struct xfarray *array, void *ptr);
int xfarray_nullify(struct xfarray *array, uint64_t idx);

/* Append an element to the array. */
static inline int xfarray_append(struct xfarray *array, void *ptr)
{
	return xfarray_store(array, array->nr, ptr);
}

uint64_t xfarray_length(struct xfarray *array);
int xfarray_load_next(struct xfarray *array, uint64_t *idx, void *rec);

typedef int (*xfarray_cmp_fn)(const void *a, const void *b);

int xfarray_sort(struct xfarray *array, xfarray_cmp_fn cmp_fn);

#define foreach_xfarray_item(array, i, rec) \
	for ((i) = 0; (i) < xfarray_length((array)); (i)++) \
		if (xfarray_load((array), (i), &(rec)) == 0 && \
		    !xfarray_is_null((array), &(rec)))

#endif /* __XFS_SCRUB_XFARRAY_H__ */
