// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_alloc.h"
#include "xfs_errortag.h"
#include "xfs_error.h"
#include "xfs_trace.h"
#include "xfs_trans.h"
#include "xfs_btree.h"
#include "xfs_sb.h"
#include "xfs_rt_resv.h"
#include "xfs_rtrmap_btree.h"

/*
 * Is the amount of space that could be allocated towards a given rt metadata
 * file at or beneath a certain threshold?
 */
static inline bool
fdblocks_under(
	struct xfs_inode	*ip,
	int64_t			rhs)
{
	/*
	 * The amount of space that can be allocated to this metadata file is
	 * the global free block count + all remaining reservation for the
	 * metadata file.  First take care of the trivial case so that we don't
	 * have to touch the per-cpu counter.
	 */
	if (ip->i_delayed_blks > rhs)
		return false;

	return __percpu_counter_compare(&ip->i_mount->m_fdblocks,
			rhs - ip->i_delayed_blks, 2048) <= 0;
}

/*
 * Are we critically low on blocks?  For now we'll define that as the number
 * of blocks we can get our hands on being less than 10% of what we reserved
 * or less than some arbitrary number (maximum btree height).
 */
bool
xfs_rt_resv_critical(
	struct xfs_mount	*mp,
	struct xfs_inode	*ip)
{
	xfs_extlen_t		btree_maxlevels;

	ASSERT(xfs_is_metadata_inode(ip));

	if (!ip)
		return false;

	trace_xfs_rt_resv_critical(ip, 0);

	/* Critically low if less than 10% or max btree height remains. */
	btree_maxlevels = xfs_btree_maxlevels(mp, XFS_BTNUM_MAX);
	return XFS_TEST_ERROR(fdblocks_under(ip, ip->i_rtresv_asked / 10) ||
			      fdblocks_under(ip, btree_maxlevels),
			      mp, XFS_ERRTAG_RT_RESV_CRITICAL);
}

static inline void
__xfs_rt_resv_free_inode(
	struct xfs_mount	*mp,
	struct xfs_inode	*ip)
{
	if (!ip)
		return;

	ASSERT(xfs_is_metadata_inode(ip));
	trace_xfs_rt_resv_free(ip, 0);

	xfs_mod_delalloc(ip->i_mount, -ip->i_delayed_blks);
	xfs_mod_fdblocks(ip->i_mount, ip->i_delayed_blks, true);
	ip->i_delayed_blks = 0;
	ip->i_rtresv_asked = 0;
}

/* Clean out a rt reservation */
void
xfs_rt_resv_free(
	struct xfs_mount	*mp)
{
	__xfs_rt_resv_free_inode(mp, mp->m_rrmapip);
}

static inline int
__xfs_rt_resv_init(
	struct xfs_mount	*mp,
	struct xfs_inode	*ip,
	xfs_filblks_t		ask)
{
	xfs_filblks_t		hidden_space;
	xfs_filblks_t		used;
	int			error;

	if (!ip || ip->i_rtresv_asked > 0)
		return 0;

	ASSERT(xfs_is_metadata_inode(ip));

	/*
	 * Space taken by all other metadata btrees are accounted on-disk as
	 * used space.  We therefore only hide the space that is reserved but
	 * not used by the trees.
	 */
	used = ip->i_d.di_nblocks;
	if (used > ask)
		ask = used;
	hidden_space = ask - used;

	error = xfs_mod_fdblocks(mp, -(int64_t)hidden_space, true);
	if (error) {
		trace_xfs_rt_resv_init_error(mp, NULLAGNUMBER, error,
				_RET_IP_);
		xfs_warn(mp,
"Space reservation for rt metadata failed.  Filesystem may run out of space.");
		return error;
	}

	xfs_mod_delalloc(mp, hidden_space);
	ip->i_delayed_blks = hidden_space;
	ip->i_rtresv_asked = ask;

	trace_xfs_rt_resv_init(ip, ask);
	return 0;
}

/* Create a rt metadata block reservation. */
int
xfs_rt_resv_init(
	struct xfs_mount	*mp)
{
	xfs_filblks_t		ask;

	ask = xfs_rtrmapbt_calc_reserves(mp);
	return __xfs_rt_resv_init(mp, mp->m_rrmapip, ask);
}

/* Allocate a block from the rt metadata file's reservation. */
void
xfs_rt_resv_alloc_extent(
	struct xfs_inode	*ip,
	struct xfs_alloc_arg	*args)
{
	int64_t			len = args->len;

	ASSERT(xfs_is_metadata_inode(ip));
	ASSERT(args->resv == XFS_AG_RESV_RTMETADATA);

	trace_xfs_rt_resv_alloc_extent(ip, args->len);

	/*
	 * Allocate the blocks from the metadata inode's block reservation
	 * and update the ondisk sb counter.
	 */
	if (ip->i_delayed_blks > 0) {
		int64_t		from_resv;

		from_resv = min_t(int64_t, len, ip->i_delayed_blks);
		ip->i_delayed_blks -= from_resv;
		xfs_mod_delalloc(ip->i_mount, -from_resv);
		xfs_trans_mod_sb(args->tp, XFS_TRANS_SB_RES_FDBLOCKS,
				-from_resv);
		len -= from_resv;
	}

	/*
	 * Any allocation in excess of the reservation requires in-core and
	 * on-disk fdblocks updates.
	 */
	if (len)
		xfs_trans_mod_sb(args->tp, XFS_TRANS_SB_FDBLOCKS, -len);

	ip->i_d.di_nblocks += args->len;
}

/* Free a block to the rt metadata file's reservation. */
void
xfs_rt_resv_free_extent(
	struct xfs_inode	*ip,
	struct xfs_trans	*tp,
	xfs_filblks_t		len)
{
	int64_t			to_resv;

	ASSERT(xfs_is_metadata_inode(ip));
	trace_xfs_rt_resv_free_extent(ip, len);

	ip->i_d.di_nblocks -= len;

	/*
	 * Add the freed blocks back into the inode's delalloc reservation
	 * until it reaches the maximum size.  Update the ondisk fdblocks only.
	 */
	to_resv = ip->i_rtresv_asked -
			(ip->i_d.di_nblocks + ip->i_delayed_blks);
	if (to_resv > 0) {
		to_resv = min_t(int64_t, to_resv, len);
		ip->i_delayed_blks += to_resv;
		xfs_mod_delalloc(ip->i_mount, to_resv);
		xfs_trans_mod_sb(tp, XFS_TRANS_SB_RES_FDBLOCKS, to_resv);
		len -= to_resv;
	}

	/*
	 * Everything else goes back to the filesystem, so update the in-core
	 * and on-disk counters.
	 */
	if (len)
		xfs_trans_mod_sb(tp, XFS_TRANS_SB_FDBLOCKS, len);
}
