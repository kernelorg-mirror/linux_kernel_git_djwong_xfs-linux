// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2016 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_REFCOUNT_H__
#define __XFS_REFCOUNT_H__

struct xfs_trans;
struct xfs_mount;
struct xfs_perag;
struct xfs_btree_cur;
struct xfs_bmbt_irec;
struct xfs_refcount_irec;

extern int xfs_refcount_lookup_le(struct xfs_btree_cur *cur,
		xfs_fsblock_t bno, int *stat);
extern int xfs_refcount_lookup_ge(struct xfs_btree_cur *cur,
		xfs_fsblock_t bno, int *stat);
extern int xfs_refcount_lookup_eq(struct xfs_btree_cur *cur,
		xfs_fsblock_t bno, int *stat);
extern int xfs_refcount_get_rec(struct xfs_btree_cur *cur,
		struct xfs_refcount_irec *irec, int *stat);

enum xfs_refcount_intent_type {
	XFS_REFCOUNT_INCREASE = 1,
	XFS_REFCOUNT_DECREASE,
	XFS_REFCOUNT_ALLOC_COW,
	XFS_REFCOUNT_FREE_COW,
};

#define XFS_REFCOUNT_INTENT_STRINGS \
	{ XFS_REFCOUNT_INCREASE,	"incr" }, \
	{ XFS_REFCOUNT_DECREASE,	"decr" }, \
	{ XFS_REFCOUNT_ALLOC_COW,	"alloc_cow" }, \
	{ XFS_REFCOUNT_FREE_COW,	"free_cow" }

struct xfs_refcount_intent {
	struct list_head			ri_list;
	enum xfs_refcount_intent_type		ri_type;
	xfs_fsblock_t				ri_startblock;
	xfs_filblks_t				ri_blockcount;
};

void xfs_refcount_increase_extent(struct xfs_trans *tp,
		struct xfs_bmbt_irec *irec);
void xfs_refcount_decrease_extent(struct xfs_trans *tp,
		struct xfs_bmbt_irec *irec);

extern void xfs_refcount_finish_one_cleanup(struct xfs_trans *tp,
		struct xfs_btree_cur *rcur, int error);
extern int xfs_refcount_finish_one(struct xfs_trans *tp,
		struct xfs_refcount_intent *refc, struct xfs_btree_cur **pcur);

extern int xfs_refcount_find_shared(struct xfs_btree_cur *cur,
		xfs_fsblock_t bno, xfs_filblks_t aglen, xfs_fsblock_t *fbno,
		xfs_filblks_t *flen, bool find_end_of_shared);

void xfs_refcount_alloc_cow_extent(struct xfs_trans *tp, xfs_fsblock_t fsb,
		xfs_filblks_t len);
void xfs_refcount_free_cow_extent(struct xfs_trans *tp, xfs_fsblock_t fsb,
		xfs_filblks_t len);
extern int xfs_refcount_recover_cow_leftovers(struct xfs_mount *mp,
		struct xfs_perag *pag);

/*
 * While we're adjusting the refcounts records of an extent, we have
 * to keep an eye on the number of extents we're dirtying -- run too
 * many in a single transaction and we'll exceed the transaction's
 * reservation and crash the fs.  Each record adds 12 bytes to the
 * log (plus any key updates) so we'll conservatively assume 32 bytes
 * per record.  We must also leave space for btree splits on both ends
 * of the range and space for the CUD and a new CUI.  Each EFI that we
 * attach to the transaction also consumes ~32 bytes.
 */
#define XFS_REFCOUNT_ITEM_OVERHEAD	32

extern int xfs_refcount_has_record(struct xfs_btree_cur *cur,
		xfs_fsblock_t bno, xfs_filblks_t len, bool *exists);
union xfs_btree_rec;
void xfs_refcount_btrec_to_irec(struct xfs_btree_cur *cur,
		const union xfs_btree_rec *rec,
		struct xfs_refcount_irec *irec);
extern int xfs_refcount_insert(struct xfs_btree_cur *cur,
		struct xfs_refcount_irec *irec, int *stat);

extern struct kmem_cache	*xfs_refcount_intent_cache;

int __init xfs_refcount_intent_init_cache(void);
void xfs_refcount_intent_destroy_cache(void);

#endif	/* __XFS_REFCOUNT_H__ */
