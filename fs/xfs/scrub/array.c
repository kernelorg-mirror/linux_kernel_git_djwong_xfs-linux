// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "scrub/array.h"
#include "scrub/scrub.h"
#include "scrub/trace.h"
#include "scrub/xfile.h"

/*
 * XFS Fixed-Size Big Memory Array
 * ===============================
 * The file-backed memory array uses a memfd "file" to store large numbers of
 * fixed-size records in memory that can be paged out.  This puts less stress
 * on the memory reclaim algorithms because memfd file pages are not pinned and
 * can be paged out; however, array access is less direct than would be in a
 * regular memory array.  Access to the array is performed via indexed get and
 * put methods, and an append method is provided for convenience.  Array
 * elements can be set to all zeroes, which means that the entry is NULL and
 * will be skipped during iteration.
 */

#define XFBMA_MAX_TEMP	(2)

/*
 * Pointer to temp space.  Because we can't access the memfd data directly, we
 * allocate a small amount of memory on the end of the xfbma to buffer array
 * items when we need space to store values temporarily.
 */
static inline void *
xfbma_temp(
	struct xfbma	*array,
	unsigned int	nr)
{
	ASSERT(nr < XFBMA_MAX_TEMP);

	return ((char *)(array + 1)) + (nr * array->obj_size);
}

/* Initialize a big memory array. */
struct xfbma *
xfbma_init(
	size_t		obj_size)
{
	struct xfbma	*array;
	struct file	*filp;
	int		error;

	filp = xfile_create("big array");
	if (!filp)
		return ERR_PTR(-ENOMEM);
	if (IS_ERR(filp))
		return ERR_CAST(filp);

	error = -ENOMEM;
	array = kmem_alloc(sizeof(struct xfbma) + (XFBMA_MAX_TEMP * obj_size),
			KM_NOFS | KM_MAYFAIL);
	if (!array)
		goto out_filp;

	array->filp = filp;
	array->obj_size = obj_size;
	array->nr = 0;
	return array;
out_filp:
	fput(filp);
	return ERR_PTR(error);
}

void
xfbma_destroy(
	struct xfbma	*array)
{
	xfile_destroy(array->filp);
	kmem_free(array);
}

/* Compute offset of array element. */
static inline loff_t
xfbma_offset(
	struct xfbma	*array,
	uint64_t	nr)
{
	if (nr >= array->nr)
		return -1;
	return nr * array->obj_size;
}

/* Get an element from the array. */
int
xfbma_get(
	struct xfbma	*array,
	uint64_t	nr,
	void		*ptr)
{
	loff_t		pos = xfbma_offset(array, nr);

	if (pos < 0) {
		ASSERT(0);
		return -ENODATA;
	}

	return xfile_io(array->filp, XFILE_IO_READ, &pos, ptr, array->obj_size);
}

/* Put an element in the array. */
int
xfbma_set(
	struct xfbma	*array,
	uint64_t	nr,
	void		*ptr)
{
	loff_t		pos = xfbma_offset(array, nr);

	if (pos < 0) {
		ASSERT(0);
		return -ENODATA;
	}

	return xfile_io(array->filp, XFILE_IO_WRITE, &pos, ptr,
			array->obj_size);
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
	void		*temp = xfbma_temp(array, 0);
	uint64_t	i;
	int		error;

	/* Find a null slot to put it in. */
	for (i = 0; i < array->nr; i++) {
		error = xfbma_get(array, i, temp);
		if (error || !xfbma_is_null(array, temp))
			continue;
		return xfbma_set(array, i, ptr);
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
	void		*temp = xfbma_temp(array, 0);
	loff_t		pos = xfbma_offset(array, nr);

	if (pos < 0) {
		ASSERT(0);
		return -ENODATA;
	}

	memset(temp, 0, array->obj_size);
	return xfile_io(array->filp, XFILE_IO_WRITE, &pos, temp,
			array->obj_size);
}

/* Append an element to the array. */
int
xfbma_append(
	struct xfbma	*array,
	void		*ptr)
{
	loff_t		pos = array->obj_size * array->nr;
	int		error;

	if (pos < 0) {
		ASSERT(0);
		return -ENODATA;
	}

	error = xfile_io(array->filp, XFILE_IO_WRITE, &pos, ptr,
			array->obj_size);
	if (error)
		return error;
	array->nr++;
	return 0;
}

/*
 * Iterate every element in this array, freeing each element as we go.
 * Array elements will be nulled out.
 */
int
xfbma_iter_del(
	struct xfbma	*array,
	xfbma_iter_fn	iter_fn,
	void		*priv)
{
	void		*temp = xfbma_temp(array, 0);
	pgoff_t		oldpagenr = 0;
	uint64_t	max_bytes;
	uint64_t	i;
	loff_t		pos;
	int		error = 0;

	max_bytes = array->nr * array->obj_size;
	for (pos = 0, i = 0; pos < max_bytes; i++) {
		pgoff_t	pagenr;

		error = xfile_io(array->filp, XFILE_IO_READ, &pos, temp,
				array->obj_size);
		if (error)
			break;
		if (xfbma_is_null(array, temp))
			goto next;
		error = iter_fn(temp, priv);
		if (error)
			break;
next:
		/* Release the previous page if possible. */
		pagenr = pos >> PAGE_SHIFT;
		if (pagenr != oldpagenr)
			xfile_discard(array->filp, oldpagenr << PAGE_SHIFT,
					pos - 1);
		oldpagenr = pagenr;
	}

	return error;
}

/* Return length of array. */
uint64_t
xfbma_length(
	struct xfbma	*array)
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
xfbma_qsort_pivot(
	struct xfbma	*array,
	xfbma_cmp_fn	cmp_fn,
	uint64_t	lo,
	uint64_t	mid,
	uint64_t	hi)
{
	void		*a = xfbma_temp(array, 0);
	void		*b = xfbma_temp(array, 1);
	int		error;

	/* if a[mid] < a[lo], swap a[mid] and a[lo]. */
	error = xfbma_get(array, mid, a);
	if (error)
		return error;
	error = xfbma_get(array, lo, b);
	if (error)
		return error;
	if (cmp_fn(a, b) < 0) {
		error = xfbma_set(array, lo, a);
		if (error)
			return error;
		error = xfbma_set(array, mid, b);
		if (error)
			return error;
	}

	/* if a[hi] < a[mid], swap a[mid] and a[hi]. */
	error = xfbma_get(array, hi, a);
	if (error)
		return error;
	error = xfbma_get(array, mid, b);
	if (error)
		return error;
	if (cmp_fn(a, b) < 0) {
		error = xfbma_set(array, mid, a);
		if (error)
			return error;
		error = xfbma_set(array, hi, b);
		if (error)
			return error;
	} else {
		goto move_front;
	}

	/* if a[mid] < a[lo], swap a[mid] and a[lo]. */
	error = xfbma_get(array, mid, a);
	if (error)
		return error;
	error = xfbma_get(array, lo, b);
	if (error)
		return error;
	if (cmp_fn(a, b) < 0) {
		error = xfbma_set(array, lo, a);
		if (error)
			return error;
		error = xfbma_set(array, mid, b);
		if (error)
			return error;
	}
move_front:
	/* move our selected pivot to a[lo] */
	error = xfbma_get(array, lo, b);
	if (error)
		return error;
	error = xfbma_get(array, mid, a);
	if (error)
		return error;
	error = xfbma_set(array, mid, b);
	if (error)
		return error;
	return xfbma_set(array, lo, a);
}

/*
 * Perform an insertion sort on a subset of the array.
 * Though insertion sort is an O(n^2) algorithm, for small set sizes it's
 * faster than quicksort's stack machine, so we let it take over for that.
 */
STATIC int
xfbma_isort(
	struct xfbma	*array,
	xfbma_cmp_fn	cmp_fn,
	uint64_t	start,
	uint64_t	end)
{
	void		*a = xfbma_temp(array, 0);
	void		*b = xfbma_temp(array, 1);
	uint64_t	tmp;
	uint64_t	i;
	uint64_t	run;
	int		error;

	/*
	 * Move the smallest element in a[start..end] to a[start].  This
	 * simplifies the loop control logic below.
	 */
	tmp = start;
	error = xfbma_get(array, tmp, b);
	if (error)
		return error;
	for (run = start + 1; run <= end; run++) {
		/* if a[run] < a[tmp], tmp = run */
		error = xfbma_get(array, run, a);
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
		error = xfbma_get(array, start, a);
		if (error)
			return error;
		error = xfbma_set(array, tmp, a);
		if (error)
			return error;
		error = xfbma_set(array, start, b);
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
		error = xfbma_get(array, run, a);
		if (error)
			return error;

		/*
		 * Find the correct place for a[run] by walking leftwards
		 * towards the start of the range until a[tmp] is no longer
		 * greater than a[run].
		 */
		tmp = run - 1;
		error = xfbma_get(array, tmp, b);
		if (error)
			return error;
		while (cmp_fn(a, b) < 0) {
			tmp--;
			error = xfbma_get(array, tmp, b);
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
			error = xfbma_get(array, i - 1, b);
			if (error)
				return error;
			error = xfbma_set(array, i, b);
			if (error)
				return error;
		}
		error = xfbma_set(array, tmp, a);
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
xfbma_sort(
	struct xfbma	*array,
	xfbma_cmp_fn	cmp_fn)
{
	int64_t		*stack;
	int64_t		*beg;
	int64_t		*end;
	void		*pivot = xfbma_temp(array, 0);
	void		*temp = xfbma_temp(array, 1);
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
		return xfbma_isort(array, cmp_fn, 0, array->nr - 1);

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
			error = xfbma_isort(array, cmp_fn, lo, hi);
			if (error)
				goto out_free;
			stack_depth--;
			continue;
		}

		/* Pick a pivot, move it to a[lo] and stash it. */
		mid = lo + ((hi - lo) / 2);
		error = xfbma_qsort_pivot(array, cmp_fn, lo, mid, hi);
		if (error)
			goto out_free;

		error = xfbma_get(array, lo, pivot);
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
			error = xfbma_get(array, hi, temp);
			if (error)
				goto out_free;
			while (cmp_fn(temp, pivot) >= 0 && lo < hi) {
				hi--;
				error = xfbma_get(array, hi, temp);
				if (error)
					goto out_free;
			}

			/* Copy that item (a[hi]) to a[lo]. */
			if (lo < hi) {
				error = xfbma_set(array, lo++, temp);
				if (error)
					goto out_free;
			}

			/*
			 * Increment lo until it finds an a[lo] greater than
			 * the pivot value.
			 */
			error = xfbma_get(array, lo, temp);
			if (error)
				goto out_free;
			while (cmp_fn(temp, pivot) <= 0 && lo < hi) {
				lo++;
				error = xfbma_get(array, lo, temp);
				if (error)
					goto out_free;
			}

			/* Copy that item (a[lo]) to a[hi]. */
			if (lo < hi) {
				error = xfbma_set(array, hi--, temp);
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
		error = xfbma_set(array, lo, pivot);
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
			return -EFSCORRUPTED;
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
	trace_xfbma_sort_stats(array->nr, max_stack_depth, max_stack_used,
			error);
	return error;
}
