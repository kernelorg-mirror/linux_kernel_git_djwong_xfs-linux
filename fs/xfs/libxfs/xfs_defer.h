// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2016 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_DEFER_H__
#define	__XFS_DEFER_H__

struct xfs_btree_cur;
struct xfs_defer_op_type;
struct xfs_defer_freezer;

/*
 * Header for deferred operation list.
 */
enum xfs_defer_ops_type {
	XFS_DEFER_OPS_TYPE_BMAP,
	XFS_DEFER_OPS_TYPE_REFCOUNT,
	XFS_DEFER_OPS_TYPE_RMAP,
	XFS_DEFER_OPS_TYPE_FREE,
	XFS_DEFER_OPS_TYPE_AGFL_FREE,
	XFS_DEFER_OPS_TYPE_MAX,
};

/*
 * Save a log intent item and a list of extents, so that we can replay
 * whatever action had to happen to the extent list and file the log done
 * item.
 */
struct xfs_defer_pending {
	struct list_head		dfp_list;	/* pending items */
	struct list_head		dfp_work;	/* work items */
	struct xfs_log_item		*dfp_intent;	/* log intent item */
	struct xfs_log_item		*dfp_done;	/* log done item */
	unsigned int			dfp_count;	/* # extent items */
	enum xfs_defer_ops_type		dfp_type;
};

void xfs_defer_add(struct xfs_trans *tp, enum xfs_defer_ops_type type,
		struct list_head *h);
int xfs_defer_finish_noroll(struct xfs_trans **tp);
int xfs_defer_finish(struct xfs_trans **tp);
void xfs_defer_cancel(struct xfs_trans *);
void xfs_defer_move(struct xfs_trans *dtp, struct xfs_trans *stp);

/* Description of a deferred type. */
struct xfs_defer_op_type {
	struct xfs_log_item *(*create_intent)(struct xfs_trans *tp,
			struct list_head *items, unsigned int count, bool sort);
	void (*abort_intent)(struct xfs_log_item *intent);
	struct xfs_log_item *(*create_done)(struct xfs_trans *tp,
			struct xfs_log_item *intent, unsigned int count);
	int (*finish_item)(struct xfs_trans *tp, struct xfs_log_item *done,
			struct list_head *item, struct xfs_btree_cur **state);
	void (*finish_cleanup)(struct xfs_trans *tp,
			struct xfs_btree_cur *state, int error);
	void (*cancel_item)(struct list_head *item);
	unsigned int		max_items;
};

extern const struct xfs_defer_op_type xfs_bmap_update_defer_type;
extern const struct xfs_defer_op_type xfs_refcount_update_defer_type;
extern const struct xfs_defer_op_type xfs_rmap_update_defer_type;
extern const struct xfs_defer_op_type xfs_extent_free_defer_type;
extern const struct xfs_defer_op_type xfs_agfl_free_defer_type;

/*
 * Deferred operation freezer.  This structure enables a dfops user to detach
 * the chain of deferred operations from a transaction so that they can be
 * run later.
 */
struct xfs_defer_freezer {
	/* List of other freezer heads. */
	struct list_head	dff_list;

	/* Deferred ops state saved from the transaction. */
	struct list_head	dff_dfops;
	unsigned int		dff_tpflags;

	/* Inodes to hold when we want to finish the deferred work items. */
#define XFS_DEFER_FREEZER_INODES	2
	unsigned int		dff_ilocks[XFS_DEFER_FREEZER_INODES];
	struct xfs_inode	*dff_inodes[XFS_DEFER_FREEZER_INODES];
};

/* Functions to freeze a chain of deferred operations for later. */
int xfs_defer_freeze(struct xfs_trans *tp, struct xfs_defer_freezer **dffp);
void xfs_defer_thaw(struct xfs_defer_freezer *dff, struct xfs_trans *tp);
void xfs_defer_freeezer_finish(struct xfs_mount *mp,
		struct xfs_defer_freezer *dff);
int xfs_defer_freezer_ijoin(struct xfs_defer_freezer *dff,
		struct xfs_inode *ip);

/* These functions must be provided by the xfs implementation. */
void xfs_defer_freezer_irele(struct xfs_defer_freezer *dff);

#endif /* __XFS_DEFER_H__ */
