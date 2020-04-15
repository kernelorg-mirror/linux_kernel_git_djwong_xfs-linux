// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SWAPEXT_H_
#define __XFS_SWAPEXT_H_ 1

/*
 * In-core information about an extent swap request between ranges of two
 * inodes.
 */
struct xfs_swapext_intent {
	/* List of other incore deferred work. */
	struct list_head	sxi_list;

	/* The two inodes we're swapping. */
	union {
		struct xfs_inode *sxi_ip1;
		xfs_ino_t	sxi_ino1;
	};
	union {
		struct xfs_inode *sxi_ip2;
		xfs_ino_t	sxi_ino2;
	};

	/* File offset range information. */
	xfs_fileoff_t		sxi_startoff1;
	xfs_fileoff_t		sxi_startoff2;
	xfs_filblks_t		sxi_blockcount;
	uint64_t		sxi_flags;

	/* Set these file sizes after the operation, unless negative. */
	xfs_fsize_t		sxi_isize1;
	xfs_fsize_t		sxi_isize2;
};

bool xfs_swapext_has_more_work(struct xfs_swapext_intent *sxi);

unsigned int xfs_swapext_reflink_prep(struct xfs_inode *ip1,
		struct xfs_inode *ip2, int whichfork, xfs_fileoff_t startoff1,
		xfs_fileoff_t startoff2, xfs_filblks_t blockcount);
void xfs_swapext_reflink_finish(struct xfs_trans *tp, struct xfs_inode *ip1,
		struct xfs_inode *ip2, unsigned int reflink_state);

void xfs_swapext_reschedule(struct xfs_trans *tpp,
		const struct xfs_swapext_intent *sxi_state);
int xfs_swapext_finish_one(struct xfs_trans *tp,
		struct xfs_swapext_intent *sxi_state);

#define XFS_SWAPEXT_SET_SIZES		(1U << 0)
#define XFS_SWAPEXT_TO_SHORTFORM2	(1U << 1)
int xfs_swapext_atomic(struct xfs_trans **tpp, struct xfs_inode *ip1,
		struct xfs_inode *ip2, int whichfork, xfs_fileoff_t startoff1,
		xfs_fileoff_t startoff2, xfs_filblks_t blockcount,
		unsigned int flags);

int xfs_swapext_deferred_bmap(struct xfs_trans **tpp, struct xfs_inode *ip1,
		struct xfs_inode *ip2, int whichfork, xfs_fileoff_t startoff1,
		xfs_fileoff_t startoff2, xfs_filblks_t blockcount,
		unsigned int flags);

#endif /* __XFS_SWAPEXT_H_ */
