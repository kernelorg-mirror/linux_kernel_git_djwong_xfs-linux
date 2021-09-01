/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
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

/*
 * Load an array element, but zero the buffer if there's no data because we
 * haven't stored to that array element yet.
 */
static inline int
xfarray_load_sparse(
	struct xfarray	*array,
	uint64_t	idx,
	void		*rec)
{
	int		error = xfarray_load(array, idx, rec);

	if (error == -ENODATA) {
		memset(rec, 0, array->obj_size);
		return 0;
	}
	return error;
}

/* Append an element to the array. */
static inline int xfarray_append(struct xfarray *array, void *ptr)
{
	return xfarray_store(array, array->nr, ptr);
}

uint64_t xfarray_length(struct xfarray *array);
int xfarray_load_next(struct xfarray *array, uint64_t *idx, void *rec);

/*
 * Iterate the non-null elements in a sparse xfarray.  Callers should
 * initialize *idx to zero before the first call; on return, it will be set to
 * one more than the index of the record that was retrieved.  Returns 1 if a
 * record was retrieved, 0 if there weren't any more records, or a negative
 * errno.
 */
static inline int
xfarray_iter(
	struct xfarray	*array,
	uint64_t	*idx,
	void		*rec)
{
	int ret = xfarray_load_next(array, idx, rec);

	if (ret == -ENODATA)
		return 0;
	if (ret == 0)
		return 1;
	return ret;
}

typedef int (*xfarray_cmp_fn)(const void *a, const void *b);

int xfarray_sort(struct xfarray *array, xfarray_cmp_fn cmp_fn);

#endif /* __XFS_SCRUB_XFARRAY_H__ */
