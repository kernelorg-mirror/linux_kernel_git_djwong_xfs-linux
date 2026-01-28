// SPDX-License-Identifier: GPL-2.0
/*
 * fuse_iext* code adapted from xfs_iext_tree.c:
 * Copyright (c) 2017 Christoph Hellwig.
 *
 * fuse_iomap_cache*lock* code adapted from xfs_inode.c:
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "fuse_i.h"
#include "fuse_trace.h"
#include "fuse_iomap_i.h"
#include "fuse_iomap.h"
#include "fuse_iomap_cache.h"
#include <linux/iomap.h>

void fuse_iomap_cache_lock_shared(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_cache *ic = fi->cache;

	down_read(&ic->ic_lock);
}

void fuse_iomap_cache_unlock_shared(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_cache *ic = fi->cache;

	up_read(&ic->ic_lock);
}

void fuse_iomap_cache_lock(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_cache *ic = fi->cache;

	down_write(&ic->ic_lock);
}

void fuse_iomap_cache_unlock(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_cache *ic = fi->cache;

	up_write(&ic->ic_lock);
}

static inline void assert_cache_locked_shared(struct fuse_iomap_cache *ic)
{
	rwsem_assert_held(&ic->ic_lock);
}

static inline void assert_cache_locked(struct fuse_iomap_cache *ic)
{
	rwsem_assert_held_write_nolockdep(&ic->ic_lock);
}

/*
 * In-core extent btree block layout:
 *
 * There are two types of blocks in the btree: leaf and inner (non-leaf) blocks.
 *
 * The leaf blocks are made up by %KEYS_PER_NODE extent records, which each
 * contain the startoffset, blockcount, startblock and unwritten extent flag.
 * See above for the exact format, followed by pointers to the previous and next
 * leaf blocks (if there are any).
 *
 * The inner (non-leaf) blocks first contain KEYS_PER_NODE lookup keys, followed
 * by an equal number of pointers to the btree blocks at the next lower level.
 *
 *		+-------+-------+-------+-------+-------+----------+----------+
 * Leaf:	| rec 1 | rec 2 | rec 3 | rec 4 | rec N | prev-ptr | next-ptr |
 *		+-------+-------+-------+-------+-------+----------+----------+
 *
 *		+-------+-------+-------+-------+-------+-------+------+-------+
 * Inner:	| key 1 | key 2 | key 3 | key N | ptr 1 | ptr 2 | ptr3 | ptr N |
 *		+-------+-------+-------+-------+-------+-------+------+-------+
 */
typedef uint64_t fuse_iext_key_t;
#define FUSE_IEXT_KEY_INVALID	(1ULL << 63)

enum {
	NODE_SIZE	= 256,
	KEYS_PER_NODE	= NODE_SIZE / (sizeof(fuse_iext_key_t) + sizeof(void *)),
	RECS_PER_LEAF	= (NODE_SIZE - (2 * sizeof(struct fuse_iext_leaf *))) /
				sizeof(struct fuse_iomap_io),
};

/* maximum length of a mapping that we're willing to cache */
#define FUSE_IOMAP_MAX_LEN	((loff_t)(1ULL << 63))

struct fuse_iext_node {
	fuse_iext_key_t		keys[KEYS_PER_NODE];
	void			*ptrs[KEYS_PER_NODE];
};

struct fuse_iext_leaf {
	struct fuse_iomap_io	recs[RECS_PER_LEAF];
	struct fuse_iext_leaf	*prev;
	struct fuse_iext_leaf	*next;
};

static uint32_t
fuse_iomap_fork_to_state(const struct fuse_iomap_cache *ic,
			 const struct fuse_iext_root *ir)
{
	ASSERT(ir == &ic->ic_write || ir == &ic->ic_read);

	if (ir == &ic->ic_write)
		return FUSE_IEXT_WRITE_MAPPING;
	return 0;
}

/* Convert bmap state flags to an inode fork. */
static struct fuse_iext_root *
fuse_iext_state_to_fork(
	struct fuse_iomap_cache	*ic,
	uint32_t		state)
{
	if (state & FUSE_IEXT_WRITE_MAPPING)
		return &ic->ic_write;
	return &ic->ic_read;
}

/* The internal iext tree record is a struct fuse_iomap_io */

static inline bool fuse_iext_rec_is_empty(const struct fuse_iomap_io *rec)
{
	return rec->length == 0;
}

static inline void fuse_iext_rec_clear(struct fuse_iomap_io *rec)
{
	memset(rec, 0, sizeof(*rec));
}

static inline void
fuse_iext_set(
	struct fuse_iomap_io		*rec,
	const struct fuse_iomap_io	*irec)
{
	ASSERT(irec->length > 0);

	*rec = *irec;
}

static inline void
fuse_iext_get(
	struct fuse_iomap_io		*irec,
	const struct fuse_iomap_io	*rec)
{
	*irec = *rec;
}

static inline uint64_t fuse_iext_count(const struct fuse_iext_root *ir)
{
	return ir->ir_bytes / sizeof(struct fuse_iomap_io);
}

static inline int fuse_iext_max_recs(const struct fuse_iext_root *ir)
{
	if (ir->ir_height == 1)
		return fuse_iext_count(ir);
	return RECS_PER_LEAF;
}

static inline struct fuse_iomap_io *cur_rec(const struct fuse_iext_cursor *cur)
{
	return &cur->leaf->recs[cur->pos];
}

static bool fuse_iext_valid(const struct fuse_iext_root *ir,
				   const struct fuse_iext_cursor *cur)
{
	if (!cur->leaf)
		return false;
	if (cur->pos < 0 || cur->pos >= fuse_iext_max_recs(ir))
		return false;
	if (fuse_iext_rec_is_empty(cur_rec(cur)))
		return false;
	return true;
}

static void *
fuse_iext_find_first_leaf(
	struct fuse_iext_root	*ir)
{
	struct fuse_iext_node	*node = ir->ir_data;
	int			height;

	if (!ir->ir_height)
		return NULL;

	for (height = ir->ir_height; height > 1; height--) {
		node = node->ptrs[0];
		ASSERT(node);
	}

	return node;
}

static void *
fuse_iext_find_last_leaf(
	struct fuse_iext_root	*ir)
{
	struct fuse_iext_node	*node = ir->ir_data;
	int			height, i;

	if (!ir->ir_height)
		return NULL;

	for (height = ir->ir_height; height > 1; height--) {
		for (i = 1; i < KEYS_PER_NODE; i++)
			if (!node->ptrs[i])
				break;
		node = node->ptrs[i - 1];
		ASSERT(node);
	}

	return node;
}

static void
fuse_iext_first(
	struct fuse_iext_root	*ir,
	struct fuse_iext_cursor	*cur)
{
	cur->pos = 0;
	cur->leaf = fuse_iext_find_first_leaf(ir);
}

static void
fuse_iext_last(
	struct fuse_iext_root	*ir,
	struct fuse_iext_cursor	*cur)
{
	int			i;

	cur->leaf = fuse_iext_find_last_leaf(ir);
	if (!cur->leaf) {
		cur->pos = 0;
		return;
	}

	for (i = 1; i < fuse_iext_max_recs(ir); i++) {
		if (fuse_iext_rec_is_empty(&cur->leaf->recs[i]))
			break;
	}
	cur->pos = i - 1;
}

static void
fuse_iext_next(
	struct fuse_iext_root	*ir,
	struct fuse_iext_cursor	*cur)
{
	if (!cur->leaf) {
		ASSERT(cur->pos <= 0 || cur->pos >= RECS_PER_LEAF);
		fuse_iext_first(ir, cur);
		return;
	}

	ASSERT(cur->pos >= 0);
	ASSERT(cur->pos < fuse_iext_max_recs(ir));

	cur->pos++;
	if (ir->ir_height > 1 && !fuse_iext_valid(ir, cur) &&
	    cur->leaf->next) {
		cur->leaf = cur->leaf->next;
		cur->pos = 0;
	}
}

static void
fuse_iext_prev(
	struct fuse_iext_root	*ir,
	struct fuse_iext_cursor	*cur)
{
	if (!cur->leaf) {
		ASSERT(cur->pos <= 0 || cur->pos >= RECS_PER_LEAF);
		fuse_iext_last(ir, cur);
		return;
	}

	ASSERT(cur->pos >= 0);
	ASSERT(cur->pos <= RECS_PER_LEAF);

recurse:
	do {
		cur->pos--;
		if (fuse_iext_valid(ir, cur))
			return;
	} while (cur->pos > 0);

	if (ir->ir_height > 1 && cur->leaf->prev) {
		cur->leaf = cur->leaf->prev;
		cur->pos = RECS_PER_LEAF;
		goto recurse;
	}
}

/*
 * Return true if the cursor points at an extent and return the extent structure
 * in gotp.  Else return false.
 */
bool
fuse_iext_get_extent(
	const struct fuse_iext_root	*ir,
	const struct fuse_iext_cursor	*cur,
	struct fuse_iomap_io		*gotp)
{
	if (!fuse_iext_valid(ir, cur))
		return false;
	fuse_iext_get(gotp, cur_rec(cur));
	return true;
}

static inline bool fuse_iext_next_extent(struct fuse_iext_root *ir,
		struct fuse_iext_cursor *cur, struct fuse_iomap_io *gotp)
{
	fuse_iext_next(ir, cur);
	return fuse_iext_get_extent(ir, cur, gotp);
}

static inline bool fuse_iext_prev_extent(struct fuse_iext_root *ir,
		struct fuse_iext_cursor *cur, struct fuse_iomap_io *gotp)
{
	fuse_iext_prev(ir, cur);
	return fuse_iext_get_extent(ir, cur, gotp);
}

/*
 * Return the extent after cur in gotp without updating the cursor.
 */
static inline bool fuse_iext_peek_next_extent(struct fuse_iext_root *ir,
		struct fuse_iext_cursor *cur, struct fuse_iomap_io *gotp)
{
	struct fuse_iext_cursor ncur = *cur;

	fuse_iext_next(ir, &ncur);
	return fuse_iext_get_extent(ir, &ncur, gotp);
}

/*
 * Return the extent before cur in gotp without updating the cursor.
 */
static inline bool fuse_iext_peek_prev_extent(struct fuse_iext_root *ir,
		struct fuse_iext_cursor *cur, struct fuse_iomap_io *gotp)
{
	struct fuse_iext_cursor ncur = *cur;

	fuse_iext_prev(ir, &ncur);
	return fuse_iext_get_extent(ir, &ncur, gotp);
}

static inline int
fuse_iext_key_cmp(
	struct fuse_iext_node	*node,
	int			n,
	loff_t			offset)
{
	if (node->keys[n] > offset)
		return 1;
	if (node->keys[n] < offset)
		return -1;
	return 0;
}

static inline int
fuse_iext_rec_cmp(
	struct fuse_iomap_io	*rec,
	loff_t			offset)
{
	if (rec->offset > offset)
		return 1;
	if (rec->offset + rec->length <= offset)
		return -1;
	return 0;
}

static void *
fuse_iext_find_level(
	struct fuse_iext_root	*ir,
	loff_t			offset,
	int			level)
{
	struct fuse_iext_node	*node = ir->ir_data;
	int			height, i;

	if (!ir->ir_height)
		return NULL;

	for (height = ir->ir_height; height > level; height--) {
		for (i = 1; i < KEYS_PER_NODE; i++)
			if (fuse_iext_key_cmp(node, i, offset) > 0)
				break;

		node = node->ptrs[i - 1];
		if (!node)
			break;
	}

	return node;
}

static int
fuse_iext_node_pos(
	struct fuse_iext_node	*node,
	loff_t			offset)
{
	int			i;

	for (i = 1; i < KEYS_PER_NODE; i++) {
		if (fuse_iext_key_cmp(node, i, offset) > 0)
			break;
	}

	return i - 1;
}

static int
fuse_iext_node_insert_pos(
	struct fuse_iext_node	*node,
	loff_t			offset)
{
	int			i;

	for (i = 0; i < KEYS_PER_NODE; i++) {
		if (fuse_iext_key_cmp(node, i, offset) > 0)
			return i;
	}

	return KEYS_PER_NODE;
}

static int
fuse_iext_node_nr_entries(
	struct fuse_iext_node	*node,
	int			start)
{
	int			i;

	for (i = start; i < KEYS_PER_NODE; i++) {
		if (node->keys[i] == FUSE_IEXT_KEY_INVALID)
			break;
	}

	return i;
}

static int
fuse_iext_leaf_nr_entries(
	struct fuse_iext_root	*ir,
	struct fuse_iext_leaf	*leaf,
	int			start)
{
	int			i;

	for (i = start; i < fuse_iext_max_recs(ir); i++) {
		if (fuse_iext_rec_is_empty(&leaf->recs[i]))
			break;
	}

	return i;
}

static inline fuse_iext_key_t
fuse_iext_leaf_key(
	struct fuse_iext_leaf	*leaf,
	int			n)
{
	return leaf->recs[n].offset;
}

static inline void *
fuse_iext_alloc_node(
	int	size)
{
	return kzalloc(size, GFP_KERNEL | __GFP_NOLOCKDEP | __GFP_NOFAIL);
}

static void
fuse_iext_grow(
	struct fuse_iext_root	*ir)
{
	struct fuse_iext_node	*node = fuse_iext_alloc_node(NODE_SIZE);
	int			i;

	if (ir->ir_height == 1) {
		struct fuse_iext_leaf *prev = ir->ir_data;

		node->keys[0] = fuse_iext_leaf_key(prev, 0);
		node->ptrs[0] = prev;
	} else  {
		struct fuse_iext_node *prev = ir->ir_data;

		ASSERT(ir->ir_height > 1);

		node->keys[0] = prev->keys[0];
		node->ptrs[0] = prev;
	}

	for (i = 1; i < KEYS_PER_NODE; i++)
		node->keys[i] = FUSE_IEXT_KEY_INVALID;

	ir->ir_data = node;
	ir->ir_height++;
}

static void
fuse_iext_update_node(
	struct fuse_iext_root	*ir,
	loff_t			old_offset,
	loff_t			new_offset,
	int			level,
	void			*ptr)
{
	struct fuse_iext_node	*node = ir->ir_data;
	int			height, i;

	for (height = ir->ir_height; height > level; height--) {
		for (i = 0; i < KEYS_PER_NODE; i++) {
			if (i > 0 && fuse_iext_key_cmp(node, i, old_offset) > 0)
				break;
			if (node->keys[i] == old_offset)
				node->keys[i] = new_offset;
		}
		node = node->ptrs[i - 1];
		ASSERT(node);
	}

	ASSERT(node == ptr);
}

static struct fuse_iext_node *
fuse_iext_split_node(
	struct fuse_iext_node	**nodep,
	int			*pos,
	int			*nr_entries)
{
	struct fuse_iext_node	*node = *nodep;
	struct fuse_iext_node	*new = fuse_iext_alloc_node(NODE_SIZE);
	const int		nr_move = KEYS_PER_NODE / 2;
	int			nr_keep = nr_move + (KEYS_PER_NODE & 1);
	int			i = 0;

	/* for sequential append operations just spill over into the new node */
	if (*pos == KEYS_PER_NODE) {
		*nodep = new;
		*pos = 0;
		*nr_entries = 0;
		goto done;
	}


	for (i = 0; i < nr_move; i++) {
		new->keys[i] = node->keys[nr_keep + i];
		new->ptrs[i] = node->ptrs[nr_keep + i];

		node->keys[nr_keep + i] = FUSE_IEXT_KEY_INVALID;
		node->ptrs[nr_keep + i] = NULL;
	}

	if (*pos >= nr_keep) {
		*nodep = new;
		*pos -= nr_keep;
		*nr_entries = nr_move;
	} else {
		*nr_entries = nr_keep;
	}
done:
	for (; i < KEYS_PER_NODE; i++)
		new->keys[i] = FUSE_IEXT_KEY_INVALID;
	return new;
}

static void
fuse_iext_insert_node(
	struct fuse_iext_root	*ir,
	fuse_iext_key_t		offset,
	void			*ptr,
	int			level)
{
	struct fuse_iext_node	*node, *new;
	int			i, pos, nr_entries;

again:
	if (ir->ir_height < level)
		fuse_iext_grow(ir);

	new = NULL;
	node = fuse_iext_find_level(ir, offset, level);
	pos = fuse_iext_node_insert_pos(node, offset);
	nr_entries = fuse_iext_node_nr_entries(node, pos);

	ASSERT(pos >= nr_entries || fuse_iext_key_cmp(node, pos, offset) != 0);
	ASSERT(nr_entries <= KEYS_PER_NODE);

	if (nr_entries == KEYS_PER_NODE)
		new = fuse_iext_split_node(&node, &pos, &nr_entries);

	/*
	 * Update the pointers in higher levels if the first entry changes
	 * in an existing node.
	 */
	if (node != new && pos == 0 && nr_entries > 0)
		fuse_iext_update_node(ir, node->keys[0], offset, level, node);

	for (i = nr_entries; i > pos; i--) {
		node->keys[i] = node->keys[i - 1];
		node->ptrs[i] = node->ptrs[i - 1];
	}
	node->keys[pos] = offset;
	node->ptrs[pos] = ptr;

	if (new) {
		offset = new->keys[0];
		ptr = new;
		level++;
		goto again;
	}
}

static struct fuse_iext_leaf *
fuse_iext_split_leaf(
	struct fuse_iext_cursor	*cur,
	int			*nr_entries)
{
	struct fuse_iext_leaf	*leaf = cur->leaf;
	struct fuse_iext_leaf	*new = fuse_iext_alloc_node(NODE_SIZE);
	const int		nr_move = RECS_PER_LEAF / 2;
	int			nr_keep = nr_move + (RECS_PER_LEAF & 1);
	int			i;

	/* for sequential append operations just spill over into the new node */
	if (cur->pos == RECS_PER_LEAF) {
		cur->leaf = new;
		cur->pos = 0;
		*nr_entries = 0;
		goto done;
	}

	for (i = 0; i < nr_move; i++) {
		new->recs[i] = leaf->recs[nr_keep + i];
		fuse_iext_rec_clear(&leaf->recs[nr_keep + i]);
	}

	if (cur->pos >= nr_keep) {
		cur->leaf = new;
		cur->pos -= nr_keep;
		*nr_entries = nr_move;
	} else {
		*nr_entries = nr_keep;
	}
done:
	if (leaf->next)
		leaf->next->prev = new;
	new->next = leaf->next;
	new->prev = leaf;
	leaf->next = new;
	return new;
}

static void
fuse_iext_alloc_root(
	struct fuse_iext_root	*ir,
	struct fuse_iext_cursor	*cur)
{
	ASSERT(ir->ir_bytes == 0);

	ir->ir_data = fuse_iext_alloc_node(sizeof(struct fuse_iomap_io));
	ir->ir_height = 1;

	/* now that we have a node step into it */
	cur->leaf = ir->ir_data;
	cur->pos = 0;
}

static void
fuse_iext_realloc_root(
	struct fuse_iext_root	*ir,
	struct fuse_iext_cursor	*cur)
{
	int64_t new_size = ir->ir_bytes + sizeof(struct fuse_iomap_io);
	void *new;

	/* account for the prev/next pointers */
	if (new_size / sizeof(struct fuse_iomap_io) == RECS_PER_LEAF)
		new_size = NODE_SIZE;

	new = krealloc(ir->ir_data, new_size,
			GFP_KERNEL | __GFP_NOLOCKDEP | __GFP_NOFAIL);
	memset(new + ir->ir_bytes, 0, new_size - ir->ir_bytes);
	ir->ir_data = new;
	cur->leaf = new;
}

/*
 * Increment the sequence counter on extent tree changes. We use WRITE_ONCE
 * here to ensure the update to the sequence counter is seen before the
 * modifications to the extent tree itself take effect.
 */
static inline void fuse_iext_inc_seq(struct fuse_iomap_cache *ic)
{
	uint64_t new_val = READ_ONCE(ic->ic_seq) + 1;

	if (new_val == FUSE_IOMAP_ALWAYS_VALID)
		new_val++;
	WRITE_ONCE(ic->ic_seq, new_val);
}

static void
fuse_iext_insert_raw(
	struct fuse_iomap_cache		*ic,
	struct fuse_iext_root		*ir,
	struct fuse_iext_cursor		*cur,
	const struct fuse_iomap_io	*irec)
{
	loff_t				offset = irec->offset;
	struct fuse_iext_leaf		*new = NULL;
	int				nr_entries, i;

	fuse_iext_inc_seq(ic);

	if (ir->ir_height == 0)
		fuse_iext_alloc_root(ir, cur);
	else if (ir->ir_height == 1)
		fuse_iext_realloc_root(ir, cur);

	nr_entries = fuse_iext_leaf_nr_entries(ir, cur->leaf, cur->pos);
	ASSERT(nr_entries <= RECS_PER_LEAF);
	ASSERT(cur->pos >= nr_entries ||
	       fuse_iext_rec_cmp(cur_rec(cur), irec->offset) != 0);

	if (nr_entries == RECS_PER_LEAF)
		new = fuse_iext_split_leaf(cur, &nr_entries);

	/*
	 * Update the pointers in higher levels if the first entry changes
	 * in an existing node.
	 */
	if (cur->leaf != new && cur->pos == 0 && nr_entries > 0) {
		fuse_iext_update_node(ir, fuse_iext_leaf_key(cur->leaf, 0),
				offset, 1, cur->leaf);
	}

	for (i = nr_entries; i > cur->pos; i--)
		cur->leaf->recs[i] = cur->leaf->recs[i - 1];
	fuse_iext_set(cur_rec(cur), irec);
	ir->ir_bytes += sizeof(struct fuse_iomap_io);

	if (new)
		fuse_iext_insert_node(ir, fuse_iext_leaf_key(new, 0), new, 2);
}

static void
fuse_iext_insert(
	struct fuse_iomap_cache		*ic,
	struct fuse_iext_cursor		*cur,
	const struct fuse_iomap_io	*irec,
	uint32_t			state)
{
	struct fuse_iext_root		*ir = fuse_iext_state_to_fork(ic, state);

	fuse_iext_insert_raw(ic, ir, cur, irec);
	trace_fuse_iext_insert(ic->ic_inode, cur, state, _RET_IP_);
}

static struct fuse_iext_node *
fuse_iext_rebalance_node(
	struct fuse_iext_node	*parent,
	int			*pos,
	struct fuse_iext_node	*node,
	int			nr_entries)
{
	/*
	 * If the neighbouring nodes are completely full, or have different
	 * parents, we might never be able to merge our node, and will only
	 * delete it once the number of entries hits zero.
	 */
	if (nr_entries == 0)
		return node;

	if (*pos > 0) {
		struct fuse_iext_node *prev = parent->ptrs[*pos - 1];
		int nr_prev = fuse_iext_node_nr_entries(prev, 0), i;

		if (nr_prev + nr_entries <= KEYS_PER_NODE) {
			for (i = 0; i < nr_entries; i++) {
				prev->keys[nr_prev + i] = node->keys[i];
				prev->ptrs[nr_prev + i] = node->ptrs[i];
			}
			return node;
		}
	}

	if (*pos + 1 < fuse_iext_node_nr_entries(parent, *pos)) {
		struct fuse_iext_node *next = parent->ptrs[*pos + 1];
		int nr_next = fuse_iext_node_nr_entries(next, 0), i;

		if (nr_entries + nr_next <= KEYS_PER_NODE) {
			/*
			 * Merge the next node into this node so that we don't
			 * have to do an additional update of the keys in the
			 * higher levels.
			 */
			for (i = 0; i < nr_next; i++) {
				node->keys[nr_entries + i] = next->keys[i];
				node->ptrs[nr_entries + i] = next->ptrs[i];
			}

			++*pos;
			return next;
		}
	}

	return NULL;
}

static void
fuse_iext_remove_node(
	struct fuse_iext_root	*ir,
	loff_t			offset,
	void			*victim)
{
	struct fuse_iext_node	*node, *parent;
	int			level = 2, pos, nr_entries, i;

	ASSERT(level <= ir->ir_height);
	node = fuse_iext_find_level(ir, offset, level);
	pos = fuse_iext_node_pos(node, offset);
again:
	ASSERT(node->ptrs[pos]);
	ASSERT(node->ptrs[pos] == victim);
	kfree(victim);

	nr_entries = fuse_iext_node_nr_entries(node, pos) - 1;
	offset = node->keys[0];
	for (i = pos; i < nr_entries; i++) {
		node->keys[i] = node->keys[i + 1];
		node->ptrs[i] = node->ptrs[i + 1];
	}
	node->keys[nr_entries] = FUSE_IEXT_KEY_INVALID;
	node->ptrs[nr_entries] = NULL;

	if (pos == 0 && nr_entries > 0) {
		fuse_iext_update_node(ir, offset, node->keys[0], level, node);
		offset = node->keys[0];
	}

	if (nr_entries >= KEYS_PER_NODE / 2)
		return;

	if (level < ir->ir_height) {
		/*
		 * If we aren't at the root yet try to find a neighbour node to
		 * merge with (or delete the node if it is empty), and then
		 * recurse up to the next level.
		 */
		level++;
		parent = fuse_iext_find_level(ir, offset, level);
		pos = fuse_iext_node_pos(parent, offset);

		ASSERT(pos != KEYS_PER_NODE);
		ASSERT(parent->ptrs[pos] == node);

		node = fuse_iext_rebalance_node(parent, &pos, node, nr_entries);
		if (node) {
			victim = node;
			node = parent;
			goto again;
		}
	} else if (nr_entries == 1) {
		/*
		 * If we are at the root and only one entry is left we can just
		 * free this node and update the root pointer.
		 */
		ASSERT(node == ir->ir_data);
		ir->ir_data = node->ptrs[0];
		ir->ir_height--;
		kfree(node);
	}
}

static void
fuse_iext_rebalance_leaf(
	struct fuse_iext_root	*ir,
	struct fuse_iext_cursor	*cur,
	struct fuse_iext_leaf	*leaf,
	loff_t			offset,
	int			nr_entries)
{
	/*
	 * If the neighbouring nodes are completely full we might never be able
	 * to merge our node, and will only delete it once the number of
	 * entries hits zero.
	 */
	if (nr_entries == 0)
		goto remove_node;

	if (leaf->prev) {
		int nr_prev = fuse_iext_leaf_nr_entries(ir, leaf->prev, 0), i;

		if (nr_prev + nr_entries <= RECS_PER_LEAF) {
			for (i = 0; i < nr_entries; i++)
				leaf->prev->recs[nr_prev + i] = leaf->recs[i];

			if (cur->leaf == leaf) {
				cur->leaf = leaf->prev;
				cur->pos += nr_prev;
			}
			goto remove_node;
		}
	}

	if (leaf->next) {
		int nr_next = fuse_iext_leaf_nr_entries(ir, leaf->next, 0), i;

		if (nr_entries + nr_next <= RECS_PER_LEAF) {
			/*
			 * Merge the next node into this node so that we don't
			 * have to do an additional update of the keys in the
			 * higher levels.
			 */
			for (i = 0; i < nr_next; i++) {
				leaf->recs[nr_entries + i] =
					leaf->next->recs[i];
			}

			if (cur->leaf == leaf->next) {
				cur->leaf = leaf;
				cur->pos += nr_entries;
			}

			offset = fuse_iext_leaf_key(leaf->next, 0);
			leaf = leaf->next;
			goto remove_node;
		}
	}

	return;
remove_node:
	if (leaf->prev)
		leaf->prev->next = leaf->next;
	if (leaf->next)
		leaf->next->prev = leaf->prev;
	fuse_iext_remove_node(ir, offset, leaf);
}

static void
fuse_iext_free_last_leaf(
	struct fuse_iext_root	*ir)
{
	ir->ir_height--;
	kfree(ir->ir_data);
	ir->ir_data = NULL;
}

static void
fuse_iext_remove(
	struct fuse_iomap_cache	*ic,
	struct fuse_iext_cursor	*cur,
	uint32_t		state)
{
	struct fuse_iext_root	*ir = fuse_iext_state_to_fork(ic, state);
	struct fuse_iext_leaf	*leaf = cur->leaf;
	loff_t			offset = fuse_iext_leaf_key(leaf, 0);
	int			i, nr_entries;

	ASSERT(ir->ir_height > 0);
	ASSERT(ir->ir_data != NULL);
	ASSERT(fuse_iext_valid(ir, cur));

	trace_fuse_iext_remove(ic->ic_inode, cur, state, _RET_IP_);

	fuse_iext_inc_seq(ic);

	nr_entries = fuse_iext_leaf_nr_entries(ir, leaf, cur->pos) - 1;
	for (i = cur->pos; i < nr_entries; i++)
		leaf->recs[i] = leaf->recs[i + 1];
	fuse_iext_rec_clear(&leaf->recs[nr_entries]);
	ir->ir_bytes -= sizeof(struct fuse_iomap_io);

	if (cur->pos == 0 && nr_entries > 0) {
		fuse_iext_update_node(ir, offset, fuse_iext_leaf_key(leaf, 0), 1,
				leaf);
		offset = fuse_iext_leaf_key(leaf, 0);
	} else if (cur->pos == nr_entries) {
		if (ir->ir_height > 1 && leaf->next)
			cur->leaf = leaf->next;
		else
			cur->leaf = NULL;
		cur->pos = 0;
	}

	if (nr_entries >= RECS_PER_LEAF / 2)
		return;

	if (ir->ir_height > 1)
		fuse_iext_rebalance_leaf(ir, cur, leaf, offset, nr_entries);
	else if (nr_entries == 0)
		fuse_iext_free_last_leaf(ir);
}

/*
 * Lookup the extent covering offset.
 *
 * If there is an extent covering offset return the extent index, and store the
 * expanded extent structure in *gotp, and the extent cursor in *cur.
 * If there is no extent covering offset, but there is an extent after it (e.g.
 * it lies in a hole) return that extent in *gotp and its cursor in *cur
 * instead.
 * If offset is beyond the last extent return false, and return an invalid
 * cursor value.
 */
static bool
fuse_iext_lookup_extent(
	struct fuse_iomap_cache	*ic,
	struct fuse_iext_root	*ir,
	loff_t			offset,
	struct fuse_iext_cursor	*cur,
	struct fuse_iomap_io	*gotp)
{
	cur->leaf = fuse_iext_find_level(ir, offset, 1);
	if (!cur->leaf) {
		cur->pos = 0;
		return false;
	}

	for (cur->pos = 0; cur->pos < fuse_iext_max_recs(ir); cur->pos++) {
		struct fuse_iomap_io *rec = cur_rec(cur);

		if (fuse_iext_rec_is_empty(rec))
			break;
		if (fuse_iext_rec_cmp(rec, offset) >= 0)
			goto found;
	}

	/* Try looking in the next node for an entry > offset */
	if (ir->ir_height == 1 || !cur->leaf->next)
		return false;
	cur->leaf = cur->leaf->next;
	cur->pos = 0;
	if (!fuse_iext_valid(ir, cur))
		return false;
found:
	fuse_iext_get(gotp, cur_rec(cur));
	return true;
}

/*
 * Returns the last extent before end, and if this extent doesn't cover
 * end, update end to the end of the extent.
 */
static bool
fuse_iext_lookup_extent_before(
	struct fuse_iomap_cache	*ic,
	struct fuse_iext_root	*ir,
	loff_t			*end,
	struct fuse_iext_cursor	*cur,
	struct fuse_iomap_io	*gotp)
{
	/* could be optimized to not even look up the next on a match.. */
	if (fuse_iext_lookup_extent(ic, ir, *end - 1, cur, gotp) &&
	    gotp->offset <= *end - 1)
		return true;
	if (!fuse_iext_prev_extent(ir, cur, gotp))
		return false;
	*end = gotp->offset + gotp->length;
	return true;
}

static void
fuse_iext_update_extent(
	struct fuse_iomap_cache	*ic,
	uint32_t		state,
	struct fuse_iext_cursor	*cur,
	struct fuse_iomap_io	*new)
{
	struct fuse_iext_root	*ir = fuse_iext_state_to_fork(ic, state);

	fuse_iext_inc_seq(ic);

	if (cur->pos == 0) {
		struct fuse_iomap_io	old;

		fuse_iext_get(&old, cur_rec(cur));
		if (new->offset != old.offset) {
			fuse_iext_update_node(ir, old.offset,
					new->offset, 1, cur->leaf);
		}
	}

	trace_fuse_iext_pre_update(ic->ic_inode, cur, state, _RET_IP_);
	fuse_iext_set(cur_rec(cur), new);
	trace_fuse_iext_post_update(ic->ic_inode, cur, state, _RET_IP_);
}

/*
 * This is a recursive function, because of that we need to be extremely
 * careful with stack usage.
 */
static void
fuse_iext_destroy_node(
	struct fuse_iext_node	*node,
	int			level)
{
	int			i;

	if (level > 1) {
		for (i = 0; i < KEYS_PER_NODE; i++) {
			if (node->keys[i] == FUSE_IEXT_KEY_INVALID)
				break;
			fuse_iext_destroy_node(node->ptrs[i], level - 1);
		}
	}

	kfree(node);
}

static void
fuse_iext_destroy(
	struct fuse_iext_root	*ir)
{
	fuse_iext_destroy_node(ir->ir_data, ir->ir_height);

	ir->ir_bytes = 0;
	ir->ir_height = 0;
	ir->ir_data = NULL;
}

static inline struct fuse_iext_root *
fuse_iext_root_ptr(
	struct fuse_iomap_cache	*ic,
	enum fuse_iomap_iodir	iodir)
{
	switch (iodir) {
	case READ_MAPPING:
		return &ic->ic_read;
	case WRITE_MAPPING:
		return &ic->ic_write;
	default:
		ASSERT(0);
		return NULL;
	}
}

static inline bool fuse_iomap_addrs_adjacent(const struct fuse_iomap_io *left,
					     const struct fuse_iomap_io *right)
{
	switch (left->type) {
	case FUSE_IOMAP_TYPE_MAPPED:
	case FUSE_IOMAP_TYPE_UNWRITTEN:
		return left->addr + left->length == right->addr;
	default:
		return left->addr  == FUSE_IOMAP_NULL_ADDR &&
		       right->addr == FUSE_IOMAP_NULL_ADDR;
	}
}

static inline bool fuse_iomap_can_merge(const struct fuse_iomap_io *left,
					const struct fuse_iomap_io *right)
{
	return (left->dev == right->dev &&
		left->offset + left->length == right->offset &&
		left->type  == right->type &&
		fuse_iomap_addrs_adjacent(left, right) &&
		left->flags == right->flags &&
		left->length + right->length <= FUSE_IOMAP_MAX_LEN);
}

static inline bool fuse_iomap_can_merge3(const struct fuse_iomap_io *left,
					 const struct fuse_iomap_io *new,
					 const struct fuse_iomap_io *right)
{
	return left->length + new->length + right->length <= FUSE_IOMAP_MAX_LEN;
}

#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG)
static void fuse_iext_check_mappings(struct fuse_iomap_cache *ic,
				     struct fuse_iext_root *ir)
{
	struct fuse_iext_cursor	icur;
	struct fuse_iomap_io	prev, got;
	struct inode		*inode = ic->ic_inode;
	struct fuse_inode	*fi = get_fuse_inode(inode);
	unsigned long long	nr = 0;
	enum fuse_iomap_iodir	iodir;

	if (ir->ir_bytes < 0 || !static_branch_unlikely(&fuse_iomap_debug))
		return;

	if (ir == &ic->ic_write)
		iodir = WRITE_MAPPING;
	else
		iodir = READ_MAPPING;

	fuse_iext_first(ir, &icur);
	if (!fuse_iext_get_extent(ir, &icur, &prev))
		return;
	trace_fuse_iext_check_mapping(ic->ic_inode, iodir, &prev, _RET_IP_);
	nr++;

	fuse_iext_next(ir, &icur);
	while (fuse_iext_get_extent(ir, &icur, &got)) {
		trace_fuse_iext_check_mapping(ic->ic_inode, iodir, &got,
					      _RET_IP_);
		if (got.length == 0 ||
		    got.offset < prev.offset + prev.length ||
		    fuse_iomap_can_merge(&prev, &got)) {
			printk(KERN_ERR "FUSE IOMAP CORRUPTION ino=%llu nr=%llu",
			       fi->orig_ino, nr);
			printk(KERN_ERR "prev: offset=%llu length=%llu type=%u flags=0x%x dev=%u addr=%llu\n",
			       prev.offset, prev.length, prev.type, prev.flags,
			       prev.dev, prev.addr);
			printk(KERN_ERR "curr: offset=%llu length=%llu type=%u flags=0x%x dev=%u addr=%llu\n",
			       got.offset, got.length, got.type, got.flags,
			       got.dev, got.addr);
		}

		prev = got;
		nr++;
		fuse_iext_next(ir, &icur);
	}
}
#else
# define fuse_iext_check_mappings(...)	((void)0)
#endif

static void
fuse_iext_del_mapping(
	struct fuse_iomap_cache	*ic,
	struct fuse_iext_root	*ir,
	struct fuse_iext_cursor	*icur,
	struct fuse_iomap_io	*got,	/* current extent entry */
	struct fuse_iomap_io	*del)	/* data to remove from extents */
{
	struct fuse_iomap_io	new;	/* new record to be inserted */
	/* first addr (fsblock aligned) past del */
	fuse_iext_key_t		del_endaddr;
	/* first offset (fsblock aligned) past del */
	fuse_iext_key_t		del_endoff = del->offset + del->length;
	/* first offset (fsblock aligned) past got */
	fuse_iext_key_t		got_endoff = got->offset + got->length;
	uint32_t		state = fuse_iomap_fork_to_state(ic, ir);

	ASSERT(del->length > 0);
	ASSERT(got->offset <= del->offset);
	ASSERT(got_endoff >= del_endoff);

	switch (del->type) {
	case FUSE_IOMAP_TYPE_MAPPED:
	case FUSE_IOMAP_TYPE_UNWRITTEN:
		del_endaddr = del->addr + del->length;
		break;
	default:
		del_endaddr = FUSE_IOMAP_NULL_ADDR;
		break;
	}

	if (got->offset == del->offset)
		state |= FUSE_IEXT_LEFT_FILLING;
	if (got_endoff == del_endoff)
		state |= FUSE_IEXT_RIGHT_FILLING;

	trace_fuse_iext_del_mapping(ic->ic_inode, state, del);
	trace_fuse_iext_del_mapping_got(ic->ic_inode, got);

	switch (state & (FUSE_IEXT_LEFT_FILLING | FUSE_IEXT_RIGHT_FILLING)) {
	case FUSE_IEXT_LEFT_FILLING | FUSE_IEXT_RIGHT_FILLING:
		/*
		 * Matches the whole extent.  Delete the entry.
		 */
		fuse_iext_remove(ic, icur, state);
		fuse_iext_prev(ir, icur);
		break;
	case FUSE_IEXT_LEFT_FILLING:
		/*
		 * Deleting the first part of the extent.
		 */
		got->offset = del_endoff;
		got->addr = del_endaddr;
		got->length -= del->length;
		fuse_iext_update_extent(ic, state, icur, got);
		break;
	case FUSE_IEXT_RIGHT_FILLING:
		/*
		 * Deleting the last part of the extent.
		 */
		got->length -= del->length;
		fuse_iext_update_extent(ic, state, icur, got);
		break;
	case 0:
		/*
		 * Deleting the middle of the extent.
		 */
		got->length = del->offset - got->offset;
		fuse_iext_update_extent(ic, state, icur, got);

		new.offset = del_endoff;
		new.length = got_endoff - del_endoff;
		new.type = got->type;
		new.flags = got->flags;
		new.addr = del_endaddr;
		new.dev = got->dev;

		fuse_iext_next(ir, icur);
		fuse_iext_insert(ic, icur, &new, state);
		break;
	}
}

int
fuse_iomap_cache_remove(
	struct inode		*inode,
	enum fuse_iomap_iodir	iodir,
	loff_t			start,		/* first file offset deleted */
	uint64_t		len)		/* length to unmap */
{
	struct fuse_iext_cursor	icur;
	struct fuse_iomap_io	got;		/* current extent record */
	struct fuse_iomap_io	del;		/* extent being deleted */
	loff_t			end;
	struct fuse_inode	*fi = get_fuse_inode(inode);
	struct fuse_iomap_cache	*ic = fi->cache;
	struct fuse_iext_root	*ir = fuse_iext_root_ptr(ic, iodir);
	bool			wasreal;
	bool			done = false;
	int			ret = 0;

	assert_cache_locked(ic);

	trace_fuse_iomap_cache_remove(&fi->inode, iodir, start, len, _RET_IP_);

	/* Fork is not active or has zero mappings */
	if (ir->ir_bytes < 0 || fuse_iext_count(ir) == 0)
		return 0;

	/* Fast shortcut if the caller wants to erase everything */
	if (start == 0 && len >= inode->i_sb->s_maxbytes) {
		fuse_iext_destroy(ir);
		return 0;
	}

	if (!len)
		goto out;

	/*
	 * If the caller wants us to remove everything to EOF, we set the end
	 * of the removal range to the maximum file offset.  We don't support
	 * unsigned file offsets.
	 */
	if (len == FUSE_IOMAP_INVAL_TO_EOF) {
		const unsigned int blocksize = i_blocksize(&fi->inode);

		len = round_up(inode->i_sb->s_maxbytes, blocksize) - start;
	}

	/*
	 * Now that we've settled len, look up the extent before the end of the
	 * range.
	 */
	end = start + len;
	if (!fuse_iext_lookup_extent_before(ic, ir, &end, &icur, &got))
		goto out;
	end--;

	while (end != -1 && end >= start) {
		/*
		 * Is the found extent after a hole in which end lives?
		 * Just back up to the previous extent, if so.
		 */
		if (got.offset > end &&
		    !fuse_iext_prev_extent(ir, &icur, &got)) {
			done = true;
			break;
		}
		/*
		 * Is the last block of this extent before the range
		 * we're supposed to delete?  If so, we're done.
		 */
		end = min_t(loff_t, end, got.offset + got.length - 1);
		if (end < start)
			break;
		/*
		 * Then deal with the (possibly delayed) allocated space
		 * we found.
		 */
		del = got;
		switch (del.type) {
		case FUSE_IOMAP_TYPE_DELALLOC:
		case FUSE_IOMAP_TYPE_HOLE:
		case FUSE_IOMAP_TYPE_INLINE:
		case FUSE_IOMAP_TYPE_PURE_OVERWRITE:
			wasreal = false;
			break;
		case FUSE_IOMAP_TYPE_MAPPED:
		case FUSE_IOMAP_TYPE_UNWRITTEN:
			wasreal = true;
			break;
		default:
			ASSERT(0);
			ret = -EFSCORRUPTED;
			goto out;
		}

		if (got.offset < start) {
			del.offset = start;
			del.length -= start - got.offset;
			if (wasreal)
				del.addr += start - got.offset;
		}
		if (del.offset + del.length > end + 1)
			del.length = end + 1 - del.offset;

		fuse_iext_del_mapping(ic, ir, &icur, &got, &del);
		end = del.offset - 1;

		/*
		 * If not done go on to the next (previous) record.
		 */
		if (end != -1 && end >= start) {
			if (!fuse_iext_get_extent(ir, &icur, &got) ||
			    (got.offset > end &&
			     !fuse_iext_prev_extent(ir, &icur, &got))) {
				done = true;
				break;
			}
		}
	}

	/* Should have removed everything */
	if (len == 0 || done || end == (loff_t)-1 || end < start)
		ret = 0;
	else
		ret = -EFSCORRUPTED;

out:
	fuse_iext_check_mappings(ic, ir);
	return ret;
}

int fuse_iomap_cache_invalidate_range(struct inode *inode, loff_t offset,
				      uint64_t length)
{
	loff_t aligned_offset;
	const unsigned int blocksize = i_blocksize(inode);
	int ret, ret2;

	if (!fuse_inode_caches_iomaps(inode))
		return 0;

	aligned_offset = round_down(offset, blocksize);
	if (length != FUSE_IOMAP_INVAL_TO_EOF) {
		length += offset - aligned_offset;
		length = round_up(length, blocksize);
	}

	fuse_iomap_cache_lock(inode);
	ret = fuse_iomap_cache_remove(inode, READ_MAPPING,
				      aligned_offset, length);
	ret2 = fuse_iomap_cache_remove(inode, WRITE_MAPPING,
				       aligned_offset, length);
	fuse_iomap_cache_unlock(inode);
	if (ret)
		return ret;
	return ret2;
}

static void
fuse_iext_add_mapping(
	struct fuse_iomap_cache		*ic,
	struct fuse_iext_root		*ir,
	struct fuse_iext_cursor		*icur,
	const struct fuse_iomap_io	*new)	/* new extent entry */
{
	struct fuse_iomap_io		left;	/* left neighbor extent entry */
	struct fuse_iomap_io		right;	/* right neighbor extent entry */
	uint32_t			state = fuse_iomap_fork_to_state(ic, ir);

	/*
	 * Check and set flags if this segment has a left neighbor.
	 */
	if (fuse_iext_peek_prev_extent(ir, icur, &left))
		state |= FUSE_IEXT_LEFT_VALID;

	/*
	 * Check and set flags if this segment has a current value.
	 * Not true if we're inserting into the "hole" at eof.
	 */
	if (fuse_iext_get_extent(ir, icur, &right))
		state |= FUSE_IEXT_RIGHT_VALID;

	/*
	 * We're inserting a real allocation between "left" and "right".
	 * Set the contiguity flags.  Don't let extents get too large.
	 */
	if ((state & FUSE_IEXT_LEFT_VALID) && fuse_iomap_can_merge(&left, new))
		state |= FUSE_IEXT_LEFT_CONTIG;

	if ((state & FUSE_IEXT_RIGHT_VALID) &&
	    fuse_iomap_can_merge(new, &right) &&
	    (!(state & FUSE_IEXT_LEFT_CONTIG) ||
	     fuse_iomap_can_merge3(&left, new, &right)))
		state |= FUSE_IEXT_RIGHT_CONTIG;

	trace_fuse_iext_add_mapping(ic->ic_inode, state, new);
	if (state & FUSE_IEXT_LEFT_VALID)
		trace_fuse_iext_add_mapping_left(ic->ic_inode, &left);
	if (state & FUSE_IEXT_RIGHT_VALID)
		trace_fuse_iext_add_mapping_right(ic->ic_inode, &right);

	/*
	 * Select which case we're in here, and implement it.
	 */
	switch (state & (FUSE_IEXT_LEFT_CONTIG | FUSE_IEXT_RIGHT_CONTIG)) {
	case FUSE_IEXT_LEFT_CONTIG | FUSE_IEXT_RIGHT_CONTIG:
		/*
		 * New allocation is contiguous with real allocations on the
		 * left and on the right.
		 * Merge all three into a single extent record.
		 */
		left.length += new->length + right.length;

		fuse_iext_remove(ic, icur, state);
		fuse_iext_prev(ir, icur);
		fuse_iext_update_extent(ic, state, icur, &left);
		break;

	case FUSE_IEXT_LEFT_CONTIG:
		/*
		 * New allocation is contiguous with a real allocation
		 * on the left.
		 * Merge the new allocation with the left neighbor.
		 */
		left.length += new->length;

		fuse_iext_prev(ir, icur);
		fuse_iext_update_extent(ic, state, icur, &left);
		break;

	case FUSE_IEXT_RIGHT_CONTIG:
		/*
		 * New allocation is contiguous with a real allocation
		 * on the right.
		 * Merge the new allocation with the right neighbor.
		 */
		right.offset = new->offset;
		right.addr = new->addr;
		right.length += new->length;
		fuse_iext_update_extent(ic, state, icur, &right);
		break;

	case 0:
		/*
		 * New allocation is not contiguous with another
		 * real allocation.
		 * Insert a new entry.
		 */
		fuse_iext_insert(ic, icur, new, state);
		break;
	}
}

static int
fuse_iomap_cache_add(
	struct inode			*inode,
	enum fuse_iomap_iodir		iodir,
	const struct fuse_iomap_io	*new)
{
	struct fuse_iext_cursor		icur;
	struct fuse_iomap_io		got;
	struct fuse_inode		*fi = get_fuse_inode(inode);
	struct fuse_iomap_cache		*ic = fi->cache;
	struct fuse_iext_root		*ir = fuse_iext_root_ptr(ic, iodir);

	assert_cache_locked(ic);
	ASSERT(new->length > 0);
	ASSERT(new->offset < inode->i_sb->s_maxbytes);

	trace_fuse_iomap_cache_add(&fi->inode, iodir, new, _RET_IP_);

	/* Mark this fork as being in use */
	if (ir->ir_bytes < 0)
		ir->ir_bytes = 0;

	if (fuse_iext_lookup_extent(ic, ir, new->offset, &icur, &got)) {
		/* make sure we only add into a hole. */
		ASSERT(got.offset > new->offset);
		ASSERT(got.offset - new->offset >= new->length);

		if (got.offset <= new->offset ||
		    got.offset - new->offset < new->length)
			return -EFSCORRUPTED;
	}

	fuse_iext_add_mapping(ic, ir, &icur, new);
	fuse_iext_check_mappings(ic, ir);
	return 0;
}

int fuse_iomap_cache_alloc(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_cache *old = NULL;
	struct fuse_iomap_cache *ic;

	ic = kzalloc(sizeof(struct fuse_iomap_cache), GFP_KERNEL);
	if (!ic)
		return -ENOMEM;

	/* Only the write mapping cache can return NOFORK */
	ic->ic_write.ir_bytes = -1;
	ic->ic_seq = FUSE_IOMAP_INIT_COOKIE;
	ic->ic_inode = inode;
	init_rwsem(&ic->ic_lock);

	if (!try_cmpxchg(&fi->cache, &old, ic)) {
		/* Someone created mapping cache before us? Free ours... */
		kfree(ic);
		return 0;
	}

	trace_fuse_iomap_cache_alloc(inode);
	return 0;
}

static void fuse_iomap_cache_purge(struct fuse_iomap_cache *ic)
{
	fuse_iext_destroy(&ic->ic_read);
	fuse_iext_destroy(&ic->ic_write);
}

void fuse_iomap_cache_free(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_cache *ic = fi->cache;

	trace_fuse_iomap_cache_free(inode);

	/*
	 * This is only called from eviction, so we cannot be racing to set or
	 * clear the pointer.
	 */
	fi->cache = NULL;

	fuse_iomap_cache_purge(ic);
	kfree(ic);
}

int
fuse_iomap_cache_upsert(
	struct inode			*inode,
	enum fuse_iomap_iodir		iodir,
	const struct fuse_iomap_io	*map)
{
	struct fuse_inode		*fi = get_fuse_inode(inode);
	struct fuse_iomap_cache		*ic = fi->cache;
	int				err;

	ASSERT(fuse_inode_caches_iomaps(inode));

	/*
	 * We interpret no write fork to mean that all writes are pure
	 * overwrites.  Avoid wasting memory if we're trying to upsert a
	 * pure overwrite.
	 */
	if (iodir == WRITE_MAPPING &&
	    map->type == FUSE_IOMAP_TYPE_PURE_OVERWRITE &&
	    ic->ic_write.ir_bytes < 0)
		return 0;

	err = fuse_iomap_cache_remove(inode, iodir, map->offset, map->length);
	if (err)
		return err;

	return fuse_iomap_cache_add(inode, iodir, map);
}

/*
 * Trim the returned map to the required bounds
 */
static void
fuse_iomap_trim(
	struct fuse_inode		*fi,
	struct fuse_iomap_lookup	*mval,
	const struct fuse_iomap_io	*got,
	loff_t				off,
	loff_t				len)
{
	struct fuse_iomap_cache		*ic = fi->cache;
	const unsigned int blocksize = i_blocksize(&fi->inode);
	const loff_t aligned_off = round_down(off, blocksize);
	const loff_t aligned_end = round_up(off + len, blocksize);
	const loff_t aligned_len = aligned_end - aligned_off;

	ASSERT(aligned_off >= got->offset);

	switch (got->type) {
	case FUSE_IOMAP_TYPE_MAPPED:
	case FUSE_IOMAP_TYPE_UNWRITTEN:
		mval->map.addr = got->addr + (aligned_off - got->offset);
		break;
	default:
		mval->map.addr = FUSE_IOMAP_NULL_ADDR;
		break;
	}
	mval->map.offset = aligned_off;
	mval->map.length = min_t(loff_t, aligned_len,
				 got->length - (aligned_off - got->offset));
	mval->map.type = got->type;
	mval->map.flags = got->flags;
	mval->map.dev = got->dev;
	mval->validity_cookie = fuse_iext_read_seq(ic);
}

enum fuse_iomap_lookup_result
fuse_iomap_cache_lookup(
	struct inode			*inode,
	enum fuse_iomap_iodir		iodir,
	loff_t				off,
	uint64_t			len,
	struct fuse_iomap_lookup	*mval)
{
	struct fuse_iomap_io		got;
	struct fuse_iext_cursor		icur;
	struct fuse_inode		*fi = get_fuse_inode(inode);
	struct fuse_iomap_cache		*ic = fi->cache;
	struct fuse_iext_root		*ir = fuse_iext_root_ptr(ic, iodir);

	assert_cache_locked_shared(ic);

	trace_fuse_iomap_cache_lookup(ic->ic_inode, iodir, off, len, _RET_IP_);

	if (ir->ir_bytes < 0) {
		/*
		 * No write fork at all means this filesystem doesn't do out of
		 * place writes.
		 */
		return LOOKUP_NOFORK;
	}

	if (!fuse_iext_lookup_extent(ic, ir, off, &icur, &got)) {
		/*
		 * Does not contain a mapping at or beyond off, which is a
		 * cache miss.
		 */
		return LOOKUP_MISS;
	}

	if (got.offset > off) {
		/*
		 * Found a mapping, but it doesn't cover the start of the
		 * range, which is effectively a miss.
		 */
		return LOOKUP_MISS;
	}

	/* Found a mapping in the cache, return it */
	fuse_iomap_trim(fi, mval, &got, off, len);

	trace_fuse_iomap_cache_lookup_result(inode, iodir, off, len, &got,
					     mval);
	return LOOKUP_HIT;
}
