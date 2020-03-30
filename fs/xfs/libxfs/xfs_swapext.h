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
	struct list_head	si_list;

	/* The two inodes we're swapping. */
	union {
		struct xfs_inode *si_ip1;
		xfs_ino_t	si_ino1;
	};
	union {
		struct xfs_inode *si_ip2;
		xfs_ino_t	si_ino2;
	};

	/* File offset range information. */
	xfs_fileoff_t		si_startoff1;
	xfs_fileoff_t		si_startoff2;
	xfs_filblks_t		si_blockcount;
	int			si_whichfork;
};

unsigned int xfs_swapext_reflink_prep(struct xfs_inode *ip1,
		struct xfs_inode *ip2, int whichfork, xfs_fileoff_t startoff1,
		xfs_fileoff_t startoff2, xfs_filblks_t blockcount);
void xfs_swapext_reflink_finish(struct xfs_trans *tp, struct xfs_inode *ip1,
		struct xfs_inode *ip2, unsigned int reflink_state);

void xfs_swapext_reschedule(struct xfs_trans *tpp,
		const struct xfs_swapext_intent *sxi_state);
int xfs_swapext_finish_one(struct xfs_trans *tp,
		struct xfs_swapext_intent *sxi_state);

int xfs_swapext_atomic(struct xfs_trans **tpp, struct xfs_inode *ip1,
		struct xfs_inode *ip2, int whichfork, xfs_fileoff_t startoff1,
		xfs_fileoff_t startoff2, xfs_filblks_t blockcount);

int xfs_swapext_deferred_bmap(struct xfs_trans **tpp, struct xfs_inode *ip1,
		struct xfs_inode *ip2, int whichfork, xfs_fileoff_t startoff1,
		xfs_fileoff_t startoff2, xfs_filblks_t blockcount);

#endif /* __XFS_SWAPEXT_H_ */
