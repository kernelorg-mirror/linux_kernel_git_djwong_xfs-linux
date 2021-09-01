// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "scrub/xfarray.h"
#include "scrub/scrub.h"
#include "scrub/trace.h"
#include "scrub/xfile.h"

/*
 * XFS Fixed-Size Big Memory Array
 * ===============================
 *
 * The file-backed memory array uses a memfd "file" to store large numbers of
 * fixed-size records in memory that can be paged out.  This puts less stress
 * on the memory reclaim algorithms because memfd file pages are not pinned and
 * can be paged out; however, array access is less direct than would be in a
 * regular memory array.  Access to the array is performed via indexed get and
 * put methods, and an append method is provided for convenience.  Array
 * elements can be set to all zeroes, which means that the entry is NULL and
 * will be skipped during iteration.
 */

/*
 * Pointer to temp space.  Because we can't access the memfd data directly, we
 * allocate a small amount of memory on the end of the xfbma to buffer array
 * items when we need space to store values temporarily.
 */
#define XFBMA_MAX_TEMP	(2)
static inline void *
xfarray_temp(
	struct xfarray	*array,
	unsigned int	idx)
{
	ASSERT(idx < XFBMA_MAX_TEMP);

	return ((char *)(array + 1)) + (idx * array->obj_size);
}

/* Compute array index given an xfile offset. */
static uint64_t
xfarray_index(
	struct xfarray	*array,
	loff_t		off)
{
	return div_u64(off, array->obj_size);
}

/* Compute xfile offset of array element. */
static inline loff_t xfarray_offset(struct xfarray *array, uint64_t idx)
{
	return idx * array->obj_size;
}

/*
 * Initialize a big memory array.  Array records cannot be larger than a
 * page, and the array cannot span more bytes than the page cache supports.
 */
int
xfarray_create(
	struct xfs_mount	*mp,
	const char		*description,
	size_t			obj_size,
	struct xfarray		**arrayp)
{
	struct xfarray		*array;
	struct xfile		*xfile;
	int			error;

	ASSERT(obj_size < PAGE_SIZE);

	error = xfile_create(mp, description, 0, &xfile);
	if (error)
		return error;

	error = -ENOMEM;
	array = kmem_alloc(sizeof(struct xfarray) + (XFBMA_MAX_TEMP * obj_size),
			KM_NOFS | KM_MAYFAIL);
	if (!array)
		goto out_xfile;

	array->xfile = xfile;
	array->obj_size = obj_size;
	array->nr = 0;
	array->max_nr = xfarray_index(array, MAX_LFS_FILESIZE);
	*arrayp = array;
	return 0;

out_xfile:
	xfile_destroy(xfile);
	return error;
}

/* Destroy the array. */
void
xfarray_destroy(
	struct xfarray	*array)
{
	xfile_destroy(array->xfile);
	kmem_free(array);
}

/* Load an element from the array. */
int
xfarray_load(
	struct xfarray	*array,
	uint64_t	idx,
	void		*ptr)
{
	if (idx >= array->nr)
		return -ENODATA;

	return xfile_obj_load(array->xfile, ptr, array->obj_size,
			xfarray_offset(array, idx));
}

/* Store an element in the array. */
int
xfarray_store(
	struct xfarray	*array,
	uint64_t	idx,
	void		*ptr)
{
	int		ret;

	if (idx >= array->max_nr)
		return -EFBIG;

	ret = xfile_obj_store(array->xfile, ptr, array->obj_size,
			xfarray_offset(array, idx));
	if (ret)
		return ret;

	array->nr = max(array->nr, idx + 1);
	return 0;
}

/* Is this array element NULL? */
bool
xfarray_is_null(
	struct xfarray	*array,
	void		*ptr)
{
	return !memchr_inv(ptr, 0, array->obj_size);
}

/* Store an element anywhere in the array that isn't NULL. */
int
xfarray_store_anywhere(
	struct xfarray	*array,
	void		*ptr)
{
	void		*temp = xfarray_temp(array, 0);
	uint64_t	i;
	int		error;

	/* Find a null slot to put it in. */
	for (i = 0; i < array->nr; i++) {
		error = xfarray_load(array, i, temp);
		if (error || !xfarray_is_null(array, temp))
			continue;
		return xfarray_store(array, i, ptr);
	}

	/* No null slots, just dump it on the end. */
	return xfarray_append(array, ptr);
}

/* NULL an element in the array. */
int
xfarray_nullify(
	struct xfarray	*array,
	uint64_t	idx)
{
	void		*temp = xfarray_temp(array, 0);

	memset(temp, 0, array->obj_size);
	return xfarray_store(array, idx, temp);
}

/* Return length of array. */
uint64_t
xfarray_length(
	struct xfarray	*array)
{
	return array->nr;
}

/*
 * Select the median value from a[lo], a[mid], and a[hi].  Put the median in
 * a[lo], the lowest in a[lo], and the highest in a[hi].  Using the median of
 * the three reduces the chances that we pick the worst case pivot value, since
 * it's likely that our array values are nearly sorted.
 */
STATIC int
xfarray_qsort_pivot(
	struct xfarray	*array,
	xfarray_cmp_fn	cmp_fn,
	uint64_t	lo,
	uint64_t	mid,
	uint64_t	hi)
{
	void		*a = xfarray_temp(array, 0);
	void		*b = xfarray_temp(array, 1);
	int		error;

	/* if a[mid] < a[lo], swap a[mid] and a[lo]. */
	error = xfarray_load(array, mid, a);
	if (error)
		return error;
	error = xfarray_load(array, lo, b);
	if (error)
		return error;
	if (cmp_fn(a, b) < 0) {
		error = xfarray_store(array, lo, a);
		if (error)
			return error;
		error = xfarray_store(array, mid, b);
		if (error)
			return error;
	}

	/* if a[hi] < a[mid], swap a[mid] and a[hi]. */
	error = xfarray_load(array, hi, a);
	if (error)
		return error;
	error = xfarray_load(array, mid, b);
	if (error)
		return error;
	if (cmp_fn(a, b) < 0) {
		error = xfarray_store(array, mid, a);
		if (error)
			return error;
		error = xfarray_store(array, hi, b);
		if (error)
			return error;
	} else {
		goto move_front;
	}

	/* if a[mid] < a[lo], swap a[mid] and a[lo]. */
	error = xfarray_load(array, mid, a);
	if (error)
		return error;
	error = xfarray_load(array, lo, b);
	if (error)
		return error;
	if (cmp_fn(a, b) < 0) {
		error = xfarray_store(array, lo, a);
		if (error)
			return error;
		error = xfarray_store(array, mid, b);
		if (error)
			return error;
	}
move_front:
	/* move our selected pivot to a[lo] */
	error = xfarray_load(array, lo, b);
	if (error)
		return error;
	error = xfarray_load(array, mid, a);
	if (error)
		return error;
	error = xfarray_store(array, mid, b);
	if (error)
		return error;
	return xfarray_store(array, lo, a);
}

/*
 * Perform an insertion sort on a subset of the array.
 * Though insertion sort is an O(n^2) algorithm, for small set sizes it's
 * faster than quicksort's stack machine, so we let it take over for that.
 */
STATIC int
xfarray_isort(
	struct xfarray	*array,
	xfarray_cmp_fn	cmp_fn,
	uint64_t	start,
	uint64_t	end)
{
	void		*a = xfarray_temp(array, 0);
	void		*b = xfarray_temp(array, 1);
	uint64_t	tmp;
	uint64_t	i;
	uint64_t	run;
	int		error;

	/*
	 * Move the smallest element in a[start..end] to a[start].  This
	 * simplifies the loop control logic below.
	 */
	tmp = start;
	error = xfarray_load(array, tmp, b);
	if (error)
		return error;
	for (run = start + 1; run <= end; run++) {
		/* if a[run] < a[tmp], tmp = run */
		error = xfarray_load(array, run, a);
		if (error)
			return error;
		if (cmp_fn(a, b) < 0) {
			tmp = run;
			memcpy(b, a, array->obj_size);
		}
	}

	/*
	 * The smallest element is a[tmp]; swap with a[start] if tmp != start.
	 * Recall that a[tmp] is already in *b.
	 */
	if (tmp != start) {
		error = xfarray_load(array, start, a);
		if (error)
			return error;
		error = xfarray_store(array, tmp, a);
		if (error)
			return error;
		error = xfarray_store(array, start, b);
		if (error)
			return error;
	}

	/*
	 * Perform an insertion sort on a[start+1..end].  We already made sure
	 * that the smallest value in the original range is now in a[start],
	 * so the inner loop should never underflow.
	 *
	 * For each a[start+2..end], make sure it's in the correct position
	 * with respect to the elements that came before it.
	 */
	for (run = start + 2; run <= end; run++) {
		error = xfarray_load(array, run, a);
		if (error)
			return error;

		/*
		 * Find the correct place for a[run] by walking leftwards
		 * towards the start of the range until a[tmp] is no longer
		 * greater than a[run].
		 */
		tmp = run - 1;
		error = xfarray_load(array, tmp, b);
		if (error)
			return error;
		while (cmp_fn(a, b) < 0) {
			tmp--;
			error = xfarray_load(array, tmp, b);
			if (error)
				return error;
		}
		tmp++;

		/*
		 * If tmp != run, then a[tmp..run-1] are all less than a[run],
		 * so right barrel roll a[tmp..run] to get this range in
		 * sorted order.
		 */
		if (tmp == run)
			continue;

		for (i = run; i >= tmp; i--) {
			error = xfarray_load(array, i - 1, b);
			if (error)
				return error;
			error = xfarray_store(array, i, b);
			if (error)
				return error;
		}
		error = xfarray_store(array, tmp, a);
		if (error)
			return error;
	}

	return 0;
}

/*
 * Sort the array elements via quicksort.  This implementation incorporates
 * four optimizations discussed in Sedgewick:
 *
 * 1. Use an explicit stack of array indicies to store the next array
 *    partition to sort.  This helps us to avoid recursion in the call stack,
 *    which is particularly expensive in the kernel.
 *
 * 2. Choose the pivot element using a median-of-three decision tree.  This
 *    reduces the probability of selecting a bad pivot value which causes
 *    worst case behavior (i.e. partition sizes of 1).  Chance are fairly good
 *    that the list is nearly sorted, so this is important.
 *
 * 3. The smaller of the two sub-partitions is pushed onto the stack to start
 *    the next level of recursion, and the larger sub-partition replaces the
 *    current stack frame.  This guarantees that we won't need more than
 *    log2(nr) stack space.
 *
 * 4. Use insertion sort for small sets since since insertion sort is faster
 *    for small, mostly sorted array segments.  In the author's experience,
 *    substituting insertion sort for arrays smaller than 4 elements yields
 *    a ~10% reduction in runtime.
 */

/*
 * Due to the use of signed indices, we can only support up to 2^63 records.
 * Files can only grow to 2^63 bytes, so this is not much of a limitation.
 */
#define QSORT_MAX_RECS		(1ULL << 63)

/*
 * For array subsets smaller than 4 elements, it's slightly faster to use
 * insertion sort than quicksort's stack machine.
 */
#define ISORT_THRESHOLD		(4)
int
xfarray_sort(
	struct xfarray	*array,
	xfarray_cmp_fn	cmp_fn)
{
	int64_t		*stack;
	int64_t		*beg;
	int64_t		*end;
	void		*pivot = xfarray_temp(array, 0);
	void		*temp = xfarray_temp(array, 1);
	int64_t		lo, mid, hi;
	const int	max_stack_depth = ilog2(array->nr) + 1;
	int		stack_depth = 0;
	int		max_stack_used = 0;
	int		error = 0;

	if (array->nr == 0)
		return 0;
	if (array->nr >= QSORT_MAX_RECS)
		return -E2BIG;
	if (array->nr <= ISORT_THRESHOLD)
		return xfarray_isort(array, cmp_fn, 0, array->nr - 1);

	/* Allocate our pointer stacks for sorting. */
	stack = kmem_alloc(sizeof(int64_t) * 2 * max_stack_depth,
			KM_NOFS | KM_MAYFAIL);
	if (!stack)
		return -ENOMEM;
	beg = stack;
	end = &stack[max_stack_depth];

	beg[0] = 0;
	end[0] = array->nr;
	while (stack_depth >= 0) {
		lo = beg[stack_depth];
		hi = end[stack_depth] - 1;

		/* Nothing left in this partition to sort; pop stack. */
		if (lo >= hi) {
			stack_depth--;
			continue;
		}

		/* Small enough for insertion sort? */
		if (hi - lo <= ISORT_THRESHOLD) {
			error = xfarray_isort(array, cmp_fn, lo, hi);
			if (error)
				goto out_free;
			stack_depth--;
			continue;
		}

		/* Pick a pivot, move it to a[lo] and stash it. */
		mid = lo + ((hi - lo) / 2);
		error = xfarray_qsort_pivot(array, cmp_fn, lo, mid, hi);
		if (error)
			goto out_free;

		error = xfarray_load(array, lo, pivot);
		if (error)
			goto out_free;

		/*
		 * Rearrange a[lo..hi] such that everything smaller than the
		 * pivot is on the left side of the range and everything larger
		 * than the pivot is on the right side of the range.
		 */
		while (lo < hi) {
			/*
			 * Decrement hi until it finds an a[hi] less than the
			 * pivot value.
			 */
			error = xfarray_load(array, hi, temp);
			if (error)
				goto out_free;
			while (cmp_fn(temp, pivot) >= 0 && lo < hi) {
				hi--;
				error = xfarray_load(array, hi, temp);
				if (error)
					goto out_free;
			}

			/* Copy that item (a[hi]) to a[lo]. */
			if (lo < hi) {
				error = xfarray_store(array, lo++, temp);
				if (error)
					goto out_free;
			}

			/*
			 * Increment lo until it finds an a[lo] greater than
			 * the pivot value.
			 */
			error = xfarray_load(array, lo, temp);
			if (error)
				goto out_free;
			while (cmp_fn(temp, pivot) <= 0 && lo < hi) {
				lo++;
				error = xfarray_load(array, lo, temp);
				if (error)
					goto out_free;
			}

			/* Copy that item (a[lo]) to a[hi]. */
			if (lo < hi) {
				error = xfarray_store(array, hi--, temp);
				if (error)
					goto out_free;
			}
		}

		/*
		 * Put our pivot value in the correct place at a[lo].  All
		 * values between a[beg[i]] and a[lo - 1] should be less than
		 * the pivot; and all values between a[lo + 1] and a[end[i]-1]
		 * should be greater than the pivot.
		 */
		error = xfarray_store(array, lo, pivot);
		if (error)
			goto out_free;

		/*
		 * Set up the pointers for the next iteration.  We push onto
		 * the stack all of the unsorted values between a[lo + 1] and
		 * a[end[i]], and we tweak the current stack frame to point to
		 * the unsorted values between a[beg[i]] and a[lo] so that
		 * those values will be sorted when we pop the stack.
		 */
		beg[stack_depth + 1] = lo + 1;
		end[stack_depth + 1] = end[stack_depth];
		end[stack_depth++] = lo;

		/* Check our stack usage. */
		max_stack_used = max(max_stack_used, stack_depth);
		if (stack_depth >= max_stack_depth) {
			ASSERT(0);
			error = -EFSCORRUPTED;
			goto out_free;
		}

		/*
		 * Always start with the smaller of the two partitions to keep
		 * the amount of recursion in check.
		 */
		if (end[stack_depth] - beg[stack_depth] >
		    end[stack_depth - 1] - beg[stack_depth - 1]) {
			swap(beg[stack_depth], beg[stack_depth - 1]);
			swap(end[stack_depth], end[stack_depth - 1]);
		}
	}

out_free:
	kfree(stack);
	trace_xfarray_sort_stats(array, max_stack_depth, max_stack_used,
			error);
	return error;
}

/*
 * Decide which array item we're going to read as part of an _iter_get.
 * @cur is the array index, and @pos is the file offset of that array index in
 * the backing xfile.  Returns ENODATA if we reach the end of the records.
 *
 * Reading from a hole in a sparse xfile causes page instantiation, so for
 * iterating a (possibly sparse) array we need to figure out if the cursor is
 * pointing at a totally uninitialized hole and move the cursor up if
 * necessary.
 */
static inline int
xfarray_find_data(
	struct xfarray	*array,
	uint64_t	*cur,
	loff_t		*pos)
{
	unsigned int	pgoff = offset_in_page(*pos);
	loff_t		end_pos = *pos + array->obj_size - 1;
	loff_t		new_pos;

	/*
	 * If the current array record is not adjacent to a page boundary, we
	 * are in the middle of the page.  We do not need to move the cursor.
	 */
	if (pgoff != 0 && pgoff + array->obj_size - 1 < PAGE_SIZE)
		return 0;

	/*
	 * Call SEEK_DATA on the last byte in the record we're about to read.
	 * If the record ends at (or crosses) the end of a page then we know
	 * that the first byte of the record is backed by pages and don't need
	 * to query it.  If instead the record begins at the start of the page
	 * then we know that querying the last byte is just as good as querying
	 * the first byte, since records cannot be larger than a page.
	 *
	 * If the call returns the same file offset, we know this record is
	 * backed by real pages.  We do not need to move the cursor.
	 */
	new_pos = xfile_seek_data(array->xfile, end_pos);
	if (new_pos == -ENXIO)
		return -ENODATA;
	if (new_pos < 0)
		return new_pos;
	if (new_pos == end_pos)
		return 0;

	/*
	 * Otherwise, SEEK_DATA told us how far up to move the file pointer to
	 * find more data.  Move the array index to the first record past the
	 * byte offset we were given.
	 */
	new_pos = roundup_64(new_pos, array->obj_size);
	*cur = xfarray_index(array, new_pos);
	*pos = xfarray_offset(array, *cur);
	return 0;
}

/*
 * Starting at *idx, fetch the next non-null array entry and advance the index
 * to set up the next _iter_get call.  Returns ENODATA if we reach the end of
 * the array.
 */
int
xfarray_load_next(
	struct xfarray	*array,
	uint64_t	*idx,
	void		*rec)
{
	uint64_t	cur = *idx;
	loff_t		off = xfarray_offset(array, cur);
	int		error;

	do {
		if (cur >= array->nr)
			return -ENODATA;

		/*
		 * Ask the backing store for the location of next possible
		 * written record, then retrieve that record.
		 */
		error = xfarray_find_data(array, &cur, &off);
		if (error)
			return error;
		error = xfarray_load(array, cur, rec);
		if (error)
			return error;

		cur++;
		off += array->obj_size;
	} while (xfarray_is_null(array, rec));

	*idx = cur;
	return 0;
}

/* How many bytes is this array consuming? */
long long
xfarray_bytes(
	struct xfarray		*array)
{
	struct xfile_stat	statbuf;
	int			error;

	error = xfile_stat(array->xfile, &statbuf);
	if (error)
		return error;

	return statbuf.bytes;
}

/* Empty the entire array. */
void
xfarray_truncate(
	struct xfarray	*array)
{
	xfile_discard(array->xfile, 0, MAX_LFS_FILESIZE);
	array->nr = 0;
}
