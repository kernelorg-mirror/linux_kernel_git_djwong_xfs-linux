/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SWAPEXT_H_
#define __XFS_SWAPEXT_H_ 1

/*
 * Decide if this filesystem supports using log items to swap file extents and
 * restart the operation if the system fails before the operation completes.
 *
 * This can be done to individual file extents by using the block mapping log
 * intent items introduced with reflink and rmap; or to entire file ranges
 * using swapext log intent items to track the overall progress across multiple
 * extent mappings.  Realtime is not supported yet.
 */
static inline bool xfs_swapext_supported(struct xfs_mount *mp)
{
	return (xfs_has_reflink(mp) || xfs_has_rmapbt(mp)) &&
	       !xfs_has_realtime(mp);
}

/*
 * In-core information about an extent swap request between ranges of two
 * inodes.
 */
struct xfs_swapext_intent {
	/* List of other incore deferred work. */
	struct list_head	sxi_list;

	/* Inodes participating in the operation. */
	struct xfs_inode	*sxi_ip1;
	struct xfs_inode	*sxi_ip2;

	/* File offset range information. */
	xfs_fileoff_t		sxi_startoff1;
	xfs_fileoff_t		sxi_startoff2;
	xfs_filblks_t		sxi_blockcount;

	/* Set these file sizes after the operation, unless negative. */
	xfs_fsize_t		sxi_isize1;
	xfs_fsize_t		sxi_isize2;

	/* XFS_SWAP_EXT_* log operation flags */
	unsigned int		sxi_flags;

	/* XFS_SWAP_EXT_OP_* flags */
	unsigned int		sxi_op_flags;
};

/* Use log intent items to track and restart the entire operation. */
#define XFS_SWAP_EXT_OP_LOGGED	(1U << 0)

/* Upgrade files to have large extent counts before proceeding. */
#define XFS_SWAP_EXT_OP_NREXT64	(1U << 1)

#define XFS_SWAP_EXT_OP_STRINGS \
	{ XFS_SWAP_EXT_OP_LOGGED,		"LOGGED" }, \
	{ XFS_SWAP_EXT_OP_NREXT64,		"NREXT64" }

static inline int
xfs_swapext_whichfork(const struct xfs_swapext_intent *sxi)
{
	if (sxi->sxi_flags & XFS_SWAP_EXT_ATTR_FORK)
		return XFS_ATTR_FORK;
	return XFS_DATA_FORK;
}

/* Parameters for a swapext request. */
struct xfs_swapext_req {
	/* Inodes participating in the operation. */
	struct xfs_inode	*ip1;
	struct xfs_inode	*ip2;

	/* File offset range information. */
	xfs_fileoff_t		startoff1;
	xfs_fileoff_t		startoff2;
	xfs_filblks_t		blockcount;

	/* Data or attr fork? */
	int			whichfork;

	/* XFS_SWAP_REQ_* operation flags */
	unsigned int		req_flags;

	/*
	 * Fields below this line are filled out by xfs_swapext_estimate;
	 * callers should initialize this part of the struct to zero.
	 */

	/*
	 * Data device blocks to be moved out of ip1, and free space needed to
	 * handle the bmbt changes.
	 */
	xfs_filblks_t		ip1_bcount;

	/*
	 * Data device blocks to be moved out of ip2, and free space needed to
	 * handle the bmbt changes.
	 */
	xfs_filblks_t		ip2_bcount;

	/* rt blocks to be moved out of ip1. */
	xfs_filblks_t		ip1_rtbcount;

	/* rt blocks to be moved out of ip2. */
	xfs_filblks_t		ip2_rtbcount;

	/* Free space needed to handle the bmbt changes */
	unsigned long long	resblks;

	/* Number of extent swaps needed to complete the operation */
	unsigned long long	nr_exchanges;
};

/* Caller has permission to use log intent items for the swapext operation. */
#define XFS_SWAP_REQ_LOGGED		(1U << 0)

/* Set the file sizes when finished. */
#define XFS_SWAP_REQ_SET_SIZES		(1U << 1)

/* Do not swap any part of the range where ip1's mapping is a hole. */
#define XFS_SWAP_REQ_SKIP_INO1_HOLES	(1U << 2)

/* Files need to be upgraded to have large extent counts. */
#define XFS_SWAP_REQ_NREXT64		(1U << 3)

/* Try to convert inode2's fork to local format, if possible. */
#define XFS_SWAP_REQ_CVT_INO2_SF	(1U << 4)

#define XFS_SWAP_REQ_FLAGS		(XFS_SWAP_REQ_LOGGED | \
					 XFS_SWAP_REQ_SET_SIZES | \
					 XFS_SWAP_REQ_SKIP_INO1_HOLES | \
					 XFS_SWAP_REQ_NREXT64 | \
					 XFS_SWAP_REQ_CVT_INO2_SF)

#define XFS_SWAP_REQ_STRINGS \
	{ XFS_SWAP_REQ_LOGGED,			"LOGGED" }, \
	{ XFS_SWAP_REQ_SET_SIZES,		"SETSIZES" }, \
	{ XFS_SWAP_REQ_SKIP_INO1_HOLES,		"SKIP_INO1_HOLES" }, \
	{ XFS_SWAP_REQ_NREXT64,			"NREXT64" }, \
	{ XFS_SWAP_REQ_CVT_INO2_SF,		"CVT_INO2_SF" }

unsigned int xfs_swapext_reflink_prep(const struct xfs_swapext_req *req);
void xfs_swapext_reflink_finish(struct xfs_trans *tp,
		const struct xfs_swapext_req *req, unsigned int reflink_state);

int xfs_swapext_estimate(struct xfs_swapext_req *req);

extern struct kmem_cache	*xfs_swapext_intent_cache;

int __init xfs_swapext_intent_init_cache(void);
void xfs_swapext_intent_destroy_cache(void);

struct xfs_swapext_intent *xfs_swapext_init_intent(
		const struct xfs_swapext_req *req, unsigned int *reflink_state);
void xfs_swapext_ensure_reflink(struct xfs_trans *tp,
		const struct xfs_swapext_intent *sxi, unsigned int reflink_state);

void xfs_swapext_schedule(struct xfs_trans *tp,
		struct xfs_swapext_intent *sxi);
int xfs_swapext_finish_one(struct xfs_trans *tp,
		struct xfs_swapext_intent *sxi);

int xfs_swapext_check_extents(struct xfs_mount *mp,
		const struct xfs_swapext_req *req);

void xfs_swapext(struct xfs_trans *tp, const struct xfs_swapext_req *req);

#endif /* __XFS_SWAPEXT_H_ */
