/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_trans.h"
#include "xfs_btree.h"
#include "xfs_error.h"
#include "xfs_buf_mem.h"
#include "xfs_btree_mem.h"
#include "xfs_ag.h"

/* Set the root of an in-memory btree. */
void
xfbtree_set_root(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr,
	int				inc)
{
	ASSERT(cur->bc_ops->type == XFS_BTREE_TYPE_MEM);

	cur->bc_mem.xfbtree->root = *ptr;
	cur->bc_mem.xfbtree->nlevels += inc;
}

/* Initialize a pointer from the in-memory btree header. */
void
xfbtree_init_ptr_from_cur(
	struct xfs_btree_cur		*cur,
	union xfs_btree_ptr		*ptr)
{
	ASSERT(cur->bc_ops->type == XFS_BTREE_TYPE_MEM);

	*ptr = cur->bc_mem.xfbtree->root;
}

/* Duplicate an in-memory btree cursor. */
struct xfs_btree_cur *
xfbtree_dup_cursor(
	struct xfs_btree_cur		*cur)
{
	struct xfs_btree_cur		*ncur;

	ASSERT(cur->bc_ops->type == XFS_BTREE_TYPE_MEM);

	ncur = xfs_btree_alloc_cursor(cur->bc_mp, cur->bc_tp, cur->bc_ops,
			cur->bc_maxlevels, cur->bc_cache);
	ncur->bc_flags = cur->bc_flags;
	ncur->bc_nlevels = cur->bc_nlevels;
	memcpy(&ncur->bc_mem, &cur->bc_mem, sizeof(cur->bc_mem));

	if (cur->bc_mem.pag)
		ncur->bc_mem.pag = xfs_perag_hold(cur->bc_mem.pag);

	return ncur;
}
