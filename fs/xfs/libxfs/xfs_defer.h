// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2016 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_DEFER_H__
#define	__XFS_DEFER_H__

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
	void				*dfp_intent;	/* log intent item */
	void				*dfp_done;	/* log done item */
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
	void (*abort_intent)(void *);
	void *(*create_done)(struct xfs_trans *, void *, unsigned int);
	int (*finish_item)(struct xfs_trans *, struct list_head *, void *,
			void **);
	void (*finish_cleanup)(struct xfs_trans *, void *, int);
	void (*cancel_item)(struct list_head *);
	int (*diff_items)(void *, struct list_head *, struct list_head *);
	void *(*create_intent)(struct xfs_trans *, uint);
	void (*log_item)(struct xfs_trans *, void *, struct list_head *);
	unsigned int		max_items;
	int (*freeze_item)(struct xfs_defer_freezer *freezer,
			struct list_head *item);
	int (*thaw_item)(struct xfs_defer_freezer *freezer,
			struct list_head *item);
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

	/*
	 * Inodes to hold when we want to finish the deferred work items.
	 * dfops freezer functions should set dff_ino.  xfs_defer_thaw will
	 * fill out the dff_inodes array, from which the dfops thaw functions
	 * can pick up the new inode pointers.
	 */
#define XFS_DEFER_FREEZER_INODES	2
	xfs_ino_t		dff_ino[XFS_DEFER_FREEZER_INODES];
	struct xfs_inode	*dff_inodes[XFS_DEFER_FREEZER_INODES];
};

/* Functions to freeze a chain of deferred operations for later. */
int xfs_defer_freeze(struct xfs_trans *tp, struct xfs_defer_freezer **dffp);
int xfs_defer_thaw(struct xfs_defer_freezer *dff, struct xfs_trans *tp);
void xfs_defer_freeezer_finish(struct xfs_mount *mp,
		struct xfs_defer_freezer *dff);
int xfs_defer_freezer_ijoin(struct xfs_defer_freezer *dff,
		struct xfs_inode *ip);
struct xfs_inode *xfs_defer_freezer_igrab(struct xfs_defer_freezer *dff,
		xfs_ino_t ino);

/* These functions must be provided by the xfs implementation. */
void xfs_defer_freezer_irele(struct xfs_defer_freezer *dff);
int xfs_defer_freezer_iget(struct xfs_defer_freezer *dff, struct xfs_trans *tp);

#endif /* __XFS_DEFER_H__ */
