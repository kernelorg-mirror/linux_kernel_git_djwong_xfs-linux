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

#endif	/* __XFS_LOG_RECOVER_H__ */
