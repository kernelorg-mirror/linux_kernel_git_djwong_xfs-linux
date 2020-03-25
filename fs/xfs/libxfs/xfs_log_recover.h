// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef	__XFS_LOG_RECOVER_H__
#define __XFS_LOG_RECOVER_H__

/*
 * Each log item type (XFS_LI_*) gets its own xlog_recover_item_type to
 * define how recovery should work for that type of log item.
 */
struct xlog_recover_item;

/* Sorting hat for log items as they're read in. */
enum xlog_recover_reorder {
	XLOG_REORDER_UNKNOWN,
	XLOG_REORDER_BUFFER_LIST,
	XLOG_REORDER_CANCEL_LIST,
	XLOG_REORDER_INODE_BUFFER_LIST,
	XLOG_REORDER_INODE_LIST,
};

typedef enum xlog_recover_reorder (*xlog_recover_reorder_fn)(
		struct xlog_recover_item *item);
typedef void (*xlog_recover_ra_pass2_fn)(struct xlog *log,
		struct xlog_recover_item *item);
typedef int (*xlog_recover_commit_pass1_fn)(struct xlog *log,
		struct xlog_recover_item *item);
typedef int (*xlog_recover_commit_pass2_fn)(struct xlog *log,
		struct list_head *buffer_list, struct xlog_recover_item *item,
		xfs_lsn_t lsn);

struct xlog_recover_item_type {
	/*
	 * These two items decide how to sort recovered log items during
	 * recovery.  If reorder_fn is non-NULL it will be called; otherwise,
	 * reorder will be used to decide.  See the comment above
	 * xlog_recover_reorder_trans for more details about what the values
	 * mean.
	 */
	enum xlog_recover_reorder	reorder;
	xlog_recover_reorder_fn		reorder_fn;

	/* Start readahead for pass2, if provided. */
	xlog_recover_ra_pass2_fn	ra_pass2_fn;

	/* Do whatever work we need to do for pass1, if provided. */
	xlog_recover_commit_pass1_fn	commit_pass1_fn;

	/*
	 * This function should do whatever work is needed for pass2 of log
	 * recovery, if provided.
	 */
	xlog_recover_commit_pass2_fn	commit_pass2_fn;
};

extern const struct xlog_recover_item_type xlog_icreate_item_type;
extern const struct xlog_recover_item_type xlog_buf_item_type;
extern const struct xlog_recover_item_type xlog_inode_item_type;
extern const struct xlog_recover_item_type xlog_dquot_item_type;
extern const struct xlog_recover_item_type xlog_quotaoff_item_type;
extern const struct xlog_recover_item_type xlog_intent_item_type;

/*
 * Macros, structures, prototypes for internal log manager use.
 */

#define XLOG_RHASH_BITS  4
#define XLOG_RHASH_SIZE	16
#define XLOG_RHASH_SHIFT 2
#define XLOG_RHASH(tid)	\
	((((uint32_t)tid)>>XLOG_RHASH_SHIFT) & (XLOG_RHASH_SIZE-1))

#define XLOG_MAX_REGIONS_IN_ITEM   (XFS_MAX_BLOCKSIZE / XFS_BLF_CHUNK / 2 + 1)


/*
 * item headers are in ri_buf[0].  Additional buffers follow.
 */
typedef struct xlog_recover_item {
	struct list_head	ri_list;
	int			ri_cnt;	/* count of regions found */
	int			ri_total;	/* total regions */
	xfs_log_iovec_t		*ri_buf;	/* ptr to regions buffer */
	const struct xlog_recover_item_type *ri_type;
} xlog_recover_item_t;

struct xlog_recover {
	struct hlist_node	r_list;
	xlog_tid_t		r_log_tid;	/* log's transaction id */
	xfs_trans_header_t	r_theader;	/* trans header for partial */
	int			r_state;	/* not needed */
	xfs_lsn_t		r_lsn;		/* xact lsn */
	struct list_head	r_itemq;	/* q for items */
};

#define ITEM_TYPE(i)	(*(unsigned short *)(i)->ri_buf[0].i_addr)

/*
 * This is the number of entries in the l_buf_cancel_table used during
 * recovery.
 */
#define	XLOG_BC_TABLE_SIZE	64

#define	XLOG_RECOVER_CRCPASS	0
#define	XLOG_RECOVER_PASS1	1
#define	XLOG_RECOVER_PASS2	2

/*
 * This structure is used during recovery to record the buf log items which
 * have been canceled and should not be replayed.
 */
struct xfs_buf_cancel {
	xfs_daddr_t		bc_blkno;
	uint			bc_len;
	int			bc_refcount;
	struct list_head	bc_list;
};

struct xfs_buf_cancel *xlog_peek_buffer_cancelled(struct xlog *log,
		xfs_daddr_t blkno, uint len, unsigned short flags);
void xlog_recover_iodone(struct xfs_buf *bp);
int xlog_check_buffer_cancelled(struct xlog *log, xfs_daddr_t blkno, uint len,
		unsigned short flags);

/* Log intent item types */

typedef int (*xlog_recover_intent_fn)(struct xlog *xlog,
		struct xlog_recover_item *item, xfs_lsn_t lsn);
typedef int (*xlog_recover_done_fn)(struct xlog *xlog,
		struct xlog_recover_item *item);
typedef int (*xlog_recover_process_intent_fn)(struct xlog *log,
		struct xfs_trans *tp, struct xfs_log_item *lip);
typedef void (*xlog_recover_cancel_intent_fn)(struct xlog *log,
		struct xfs_log_item *lip);

struct xlog_recover_intent_type {
	/*
	 * This function should parse the recovered log item (which will be an
	 * intent log item) to construct an in-core log intent item and insert
	 * it into the AIL.  The in-core log intent item should have 1 refcount
	 * so that ->recover_done or ->cancel_intent can drop it.
	 */
	xlog_recover_intent_fn		recover_intent;

	/*
	 * This function should do the actual work of replaying an unfinished
	 * log intent item.
	 */
	xlog_recover_process_intent_fn	process_intent;

	/*
	 * This function is called to release an incore log intent item if
	 * recovery fails.
	 */
	xlog_recover_cancel_intent_fn	cancel_intent;

	/*
	 * This function should parse the recovered log item (which will be an
	 * intent done log item) to find the id of the corresponding intent log
	 * item.  Find the incore item in the AIL and release it.
	 */
	xlog_recover_done_fn		recover_done;
};

extern const struct xlog_recover_intent_type xlog_recover_extfree_type;
extern const struct xlog_recover_intent_type xlog_recover_rmap_type;

#endif	/* __XFS_LOG_RECOVER_H__ */
