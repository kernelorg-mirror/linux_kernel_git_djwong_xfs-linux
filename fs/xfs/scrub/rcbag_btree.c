// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_btree.h"
#include "xfs_btree_mem.h"
#include "xfs_error.h"
#include "scrub/xfile.h"
#include "scrub/xfbtree.h"
#include "scrub/rcbag_btree.h"
#include "scrub/trace.h"

static struct kmem_cache	*rcbagbt_cur_cache;

STATIC void
rcbagbt_init_key_from_rec(
	union xfs_btree_key		*key,
	const union xfs_btree_rec	*rec)
{
	struct rcbag_key	*bag_key = (struct rcbag_key *)key;
	const struct rcbag_rec	*bag_rec = (const struct rcbag_rec *)rec;

	BUILD_BUG_ON(sizeof(struct rcbag_key) > sizeof(union xfs_btree_key));
	BUILD_BUG_ON(sizeof(struct rcbag_rec) > sizeof(union xfs_btree_rec));

	bag_key->rbg_startblock = bag_rec->rbg_startblock;
	bag_key->rbg_blockcount = bag_rec->rbg_blockcount;
}

STATIC void
rcbagbt_init_rec_from_cur(
	struct xfs_btree_cur	*cur,
	union xfs_btree_rec	*rec)
{
	struct rcbag_rec	*bag_rec = (struct rcbag_rec *)rec;
	struct rcbag_rec	*bag_irec = (struct rcbag_rec *)&cur->bc_rec;

	bag_rec->rbg_startblock = bag_irec->rbg_startblock;
	bag_rec->rbg_blockcount = bag_irec->rbg_blockcount;
	bag_rec->rbg_refcount = bag_irec->rbg_refcount;
}

STATIC int64_t
rcbagbt_key_diff(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key)
{
	struct rcbag_rec		*rec = (struct rcbag_rec *)&cur->bc_rec;
	const struct rcbag_key		*kp = (const struct rcbag_key *)key;

	if (kp->rbg_startblock > rec->rbg_startblock)
		return 1;
	if (kp->rbg_startblock < rec->rbg_startblock)
		return -1;

	if (kp->rbg_blockcount > rec->rbg_blockcount)
		return 1;
	if (kp->rbg_blockcount < rec->rbg_blockcount)
		return -1;

	return 0;
}

STATIC int64_t
rcbagbt_diff_two_keys(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*k1,
	const union xfs_btree_key	*k2,
	const union xfs_btree_key	*mask)
{
	const struct rcbag_key		*kp1 = (const struct rcbag_key *)k1;
	const struct rcbag_key		*kp2 = (const struct rcbag_key *)k2;

	ASSERT(mask == NULL);

	if (kp1->rbg_startblock > kp2->rbg_startblock)
		return 1;
	if (kp1->rbg_startblock < kp2->rbg_startblock)
		return -1;

	if (kp1->rbg_blockcount > kp2->rbg_blockcount)
		return 1;
	if (kp1->rbg_blockcount < kp2->rbg_blockcount)
		return -1;

	return 0;
}

STATIC int
rcbagbt_keys_inorder(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*k1,
	const union xfs_btree_key	*k2)
{
	const struct rcbag_key		*kp1 = (const struct rcbag_key *)k1;
	const struct rcbag_key		*kp2 = (const struct rcbag_key *)k2;

	if (kp1->rbg_startblock > kp2->rbg_startblock)
		return 0;
	if (kp1->rbg_startblock < kp2->rbg_startblock)
		return 1;

	if (kp1->rbg_blockcount > kp2->rbg_blockcount)
		return 0;
	if (kp1->rbg_blockcount < kp2->rbg_blockcount)
		return 1;

	return 0;
}

STATIC int
rcbagbt_recs_inorder(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_rec	*r1,
	const union xfs_btree_rec	*r2)
{
	const struct rcbag_rec		*rp1 = (const struct rcbag_rec *)r1;
	const struct rcbag_rec		*rp2 = (const struct rcbag_rec *)r2;

	if (rp1->rbg_startblock > rp2->rbg_startblock)
		return 0;
	if (rp1->rbg_startblock < rp2->rbg_startblock)
		return 1;

	if (rp1->rbg_blockcount > rp2->rbg_blockcount)
		return 0;
	if (rp1->rbg_blockcount < rp2->rbg_blockcount)
		return 1;

	return 0;
}

static xfs_failaddr_t
rcbagbt_verify(
	struct xfs_buf		*bp)
{
	struct xfs_mount	*mp = bp->b_mount;
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	xfs_failaddr_t		fa;
	unsigned int		level;

	if (!xfs_verify_magic(bp, block->bb_magic))
		return __this_address;

	fa = xfs_btree_lblock_v5hdr_verify(bp, XFS_RMAP_OWN_UNKNOWN);
	if (fa)
		return fa;

	level = be16_to_cpu(block->bb_level);
	if (level >= rcbagbt_maxlevels_possible())
		return __this_address;

	return xfbtree_lblock_verify(bp,
			rcbagbt_maxrecs(mp, xfo_to_b(1), level == 0));
}

static void
rcbagbt_rw_verify(
	struct xfs_buf	*bp)
{
	xfs_failaddr_t	fa = rcbagbt_verify(bp);

	if (fa)
		xfs_verifier_error(bp, -EFSCORRUPTED, fa);
}

/* skip crc checks on in-memory btrees to save time */
static const struct xfs_buf_ops rcbagbt_mem_buf_ops = {
	.name			= "rcbagbt_mem",
	.magic			= { 0, cpu_to_be32(RCBAG_MAGIC) },
	.verify_read		= rcbagbt_rw_verify,
	.verify_write		= rcbagbt_rw_verify,
	.verify_struct		= rcbagbt_verify,
};

static const struct xfs_btree_ops rcbagbt_mem_ops = {
	.rec_len		= sizeof(struct rcbag_rec),
	.key_len		= sizeof(struct rcbag_key),
	.lru_refs		= 1,
	.geom_flags		= XFS_BTREE_CRC_BLOCKS | XFS_BTREE_LONG_PTRS |
				  XFS_BTREE_IN_XFILE,

	.dup_cursor		= xfbtree_dup_cursor,
	.set_root		= xfbtree_set_root,
	.alloc_block		= xfbtree_alloc_block,
	.free_block		= xfbtree_free_block,
	.get_minrecs		= xfbtree_get_minrecs,
	.get_maxrecs		= xfbtree_get_maxrecs,
	.init_key_from_rec	= rcbagbt_init_key_from_rec,
	.init_rec_from_cur	= rcbagbt_init_rec_from_cur,
	.init_ptr_from_cur	= xfbtree_init_ptr_from_cur,
	.key_diff		= rcbagbt_key_diff,
	.buf_ops		= &rcbagbt_mem_buf_ops,
	.diff_two_keys		= rcbagbt_diff_two_keys,
	.keys_inorder		= rcbagbt_keys_inorder,
	.recs_inorder		= rcbagbt_recs_inorder,
};

/* Create a cursor for an in-memory btree. */
struct xfs_btree_cur *
rcbagbt_mem_cursor(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	struct xfs_buf		*head_bp,
	struct xfbtree		*xfbtree)
{
	struct xfs_btree_cur	*cur;

	cur = xfs_btree_alloc_cursor(mp, tp, XFS_BTNUM_RCBAG, &rcbagbt_mem_ops,
			rcbagbt_maxlevels_possible(), rcbagbt_cur_cache);

	cur->bc_mem.xfbtree = xfbtree;
	cur->bc_mem.head_bp = head_bp;
	cur->bc_nlevels = xfs_btree_mem_head_nlevels(head_bp);
	return cur;
}

/* Create an in-memory refcount bag btree. */
int
rcbagbt_mem_create(
	struct xfs_mount	*mp,
	struct xfs_buftarg	*target,
	struct xfbtree		**xfbtreep)
{
	struct xfbtree_config	cfg = {
		.btree_ops	= &rcbagbt_mem_ops,
		.target		= target,
	};

	return xfbtree_create(mp, &cfg, xfbtreep);
}

/* Calculate number of records in a refcount bag btree block. */
static inline unsigned int
rcbagbt_block_maxrecs(
	unsigned int		blocklen,
	bool			leaf)
{
	if (leaf)
		return blocklen / sizeof(struct rcbag_rec);
	return blocklen /
		(sizeof(struct rcbag_key) + sizeof(rcbag_ptr_t));
}

/*
 * Calculate number of records in an refcount bag btree block.
 */
unsigned int
rcbagbt_maxrecs(
	struct xfs_mount	*mp,
	unsigned int		blocklen,
	bool			leaf)
{
	blocklen -= RCBAG_BLOCK_LEN;
	return rcbagbt_block_maxrecs(blocklen, leaf);
}

#define RCBAGBT_INIT_MINRECS(minrecs) \
	do { \
		unsigned int		blocklen; \
\
		blocklen = PAGE_SIZE - XFS_BTREE_LBLOCK_CRC_LEN; \
\
		minrecs[0] = rcbagbt_block_maxrecs(blocklen, true) / 2; \
		minrecs[1] = rcbagbt_block_maxrecs(blocklen, false) / 2; \
	} while (0)

/* Compute the max possible height for refcount bag btrees. */
unsigned int
rcbagbt_maxlevels_possible(void)
{
	unsigned int		minrecs[2];

	RCBAGBT_INIT_MINRECS(minrecs);
	return xfs_btree_space_to_height(minrecs, ULLONG_MAX);
}

/* Calculate the refcount bag btree size for some records. */
unsigned long long
rcbagbt_calc_size(
	unsigned long long	nr_records)
{
	unsigned int		minrecs[2];

	RCBAGBT_INIT_MINRECS(minrecs);
	return xfs_btree_calc_size(minrecs, nr_records);
}

int __init
rcbagbt_init_cur_cache(void)
{
	rcbagbt_cur_cache = kmem_cache_create("xfs_rcbagbt_cur",
			xfs_btree_cur_sizeof(rcbagbt_maxlevels_possible()),
			0, 0, NULL);

	if (!rcbagbt_cur_cache)
		return -ENOMEM;
	return 0;
}

void
rcbagbt_destroy_cur_cache(void)
{
	kmem_cache_destroy(rcbagbt_cur_cache);
	rcbagbt_cur_cache = NULL;
}
