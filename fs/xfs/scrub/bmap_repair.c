// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_btree.h"
#include "xfs_bit.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_inode.h"
#include "xfs_inode_fork.h"
#include "xfs_alloc.h"
#include "xfs_rtalloc.h"
#include "xfs_bmap.h"
#include "xfs_bmap_util.h"
#include "xfs_bmap_btree.h"
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_refcount.h"
#include "xfs_quota.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/bitmap.h"
#include "scrub/array.h"

/*
 * Inode Fork Block Mapping (BMBT) Repair
 * ======================================
 *
 * Gather all the rmap records for the inode and fork we're fixing, reset the
 * incore fork, then recreate the btree.
 */
struct xrep_bmap {
	/* Old bmbt blocks */
	struct xbitmap		old_bmbt_blocks;

	/* New fork. */
	struct xrep_newbt	new_fork_info;

	/* List of new bmap records. */
	struct xfbma		*bmap_records;

	struct xfs_scrub	*sc;

	/* How many blocks did we find allocated to this file? */
	xfs_rfsblock_t		nblocks;

	/* How many bmbt blocks did we find for this fork? */
	xfs_rfsblock_t		old_bmbt_block_count;

	/* get_data()'s position in the free space record array. */
	uint64_t		iter;

	/* Which fork are we fixing? */
	int			whichfork;
};

/* Record extents that belong to this inode's fork. */
STATIC int
xrep_bmap_walk_rmap(
	struct xfs_btree_cur	*cur,
	struct xfs_rmap_irec	*rec,
	void			*priv)
{
	struct xrep_bmap	*rb = priv;
	struct xfs_bmbt_rec	rbe;
	struct xfs_bmbt_irec	irec;
	struct xfs_mount	*mp = cur->bc_mp;
	xfs_fsblock_t		fsbno;
	int			error = 0;

	if (xchk_should_terminate(rb->sc, &error))
		return error;

	/* Skip extents which are not owned by this inode and fork. */
	if (rec->rm_owner != rb->sc->ip->i_ino)
		return 0;

	rb->nblocks += rec->rm_blockcount;

	/* If this rmap isn't for the fork we want, we're done. */
	if (rb->whichfork == XFS_DATA_FORK &&
	    (rec->rm_flags & XFS_RMAP_ATTR_FORK))
		return 0;
	if (rb->whichfork == XFS_ATTR_FORK &&
	    !(rec->rm_flags & XFS_RMAP_ATTR_FORK))
		return 0;

	/* Remember any old bmbt blocks we find so we can delete them later. */
	if (rec->rm_flags & XFS_RMAP_BMBT_BLOCK) {
		fsbno = XFS_AGB_TO_FSB(mp, cur->bc_private.a.agno,
				rec->rm_startblock);
		rb->old_bmbt_block_count += rec->rm_blockcount;
		return xbitmap_set(&rb->old_bmbt_blocks, fsbno,
				rec->rm_blockcount);
	}

	/* Remember this rmap as a series of bmap records. */
	irec.br_startoff = rec->rm_offset;
	irec.br_startblock = XFS_AGB_TO_FSB(mp, cur->bc_private.a.agno,
					rec->rm_startblock);
	if (rec->rm_flags & XFS_RMAP_UNWRITTEN)
		irec.br_state = XFS_EXT_UNWRITTEN;
	else
		irec.br_state = XFS_EXT_NORM;

	do {
		xfs_extlen_t len = min_t(xfs_filblks_t, rec->rm_blockcount,
					 MAXEXTLEN);

		irec.br_blockcount = len;
		xfs_bmbt_disk_set_all(&rbe, &irec);

		trace_xrep_bmap_found(rb->sc->ip, rb->whichfork, &irec);

		error = xfbma_append(rb->bmap_records, &rbe);

		irec.br_startblock += len;
		irec.br_startoff += len;
		rec->rm_blockcount -= len;
	} while (error == 0 && rec->rm_blockcount > 0);

	return error;
}

/* Compare two bmap extents. */
static int
xrep_bmap_extent_cmp(
	const void			*a,
	const void			*b)
{
	xfs_fileoff_t			ao;
	xfs_fileoff_t			bo;

	ao = xfs_bmbt_disk_get_startoff((struct xfs_bmbt_rec *)a);
	bo = xfs_bmbt_disk_get_startoff((struct xfs_bmbt_rec *)b);

	if (ao > bo)
		return 1;
	else if (ao < bo)
		return -1;
	return 0;
}

/* Scan one AG for reverse mappings that we can turn into extent maps. */
STATIC int
xrep_bmap_scan_ag(
	struct xrep_bmap	*rb,
	xfs_agnumber_t		agno)
{
	struct xfs_scrub	*sc = rb->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_buf		*agf_bp = NULL;
	struct xfs_btree_cur	*cur;
	int			error;

	error = xfs_alloc_read_agf(mp, sc->tp, agno, 0, &agf_bp);
	if (error)
		return error;
	if (!agf_bp)
		return -ENOMEM;
	cur = xfs_rmapbt_init_cursor(mp, sc->tp, agf_bp, agno);
	error = xfs_rmap_query_all(cur, xrep_bmap_walk_rmap, rb);
	xfs_btree_del_cursor(cur, error);
	xfs_trans_brelse(sc->tp, agf_bp);
	return error;
}

/*
 * Collect block mappings for this fork of this inode and decide if we have
 * enough space to rebuild.  Caller is responsible for cleaning up the list if
 * anything goes wrong.
 */
STATIC int
xrep_bmap_find_mappings(
	struct xrep_bmap	*rb)
{
	struct xfs_scrub	*sc = rb->sc;
	xfs_agnumber_t		agno;
	int			error = 0;

	/* Iterate the rmaps for extents. */
	for (agno = 0; agno < sc->mp->m_sb.sb_agcount; agno++) {
		error = xrep_bmap_scan_ag(rb, agno);
		if (error)
			return error;
	}

	return 0;
}

/* Retrieve bmap data for bulk load. */
STATIC int
xrep_bmap_get_data(
	struct xfs_btree_cur	*cur,
	void			*priv)
{
	struct xfs_bmbt_rec	rec;
	struct xfs_bmbt_irec	*irec = &cur->bc_rec.b;
	struct xrep_bmap	*rb = priv;
	int			error;

	do {
		error = xfbma_get(rb->bmap_records, rb->iter++, &rec);
	} while (error == 0 && xfbma_is_null(rb->bmap_records, &rec));

	xfs_bmbt_disk_get_all(&rec, irec);
	return error;
}

/* Feed one of the new btree blocks to the bulk loader. */
STATIC int
xrep_bmap_alloc_block(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	void			*priv)
{
	struct xrep_bmap        *rb = priv;

	return xrep_newbt_alloc_block(cur, &rb->new_fork_info, ptr);
}

/* Figure out how much space we need to create the incore btree root block. */
STATIC size_t
xrep_bmap_iroot_size(
	struct xfs_btree_cur	*cur,
	unsigned int		nr_this_level,
	void			*priv)
{
	return XFS_BMAP_BROOT_SPACE_CALC(cur->bc_mp, nr_this_level);
}

/* Update the inode counters. */
STATIC int
xrep_bmap_reset_counters(
	struct xrep_bmap	*rb)
{
	struct xfs_scrub	*sc = rb->sc;
	struct xbtree_ifakeroot	*ifake = &rb->new_fork_info.ifake;
	int64_t			delta;
	int			error;

	/*
	 * Update the inode block counts to reflect the extents we found in the
	 * rmapbt.
	 */
	delta = ifake->if_blocks - rb->old_bmbt_block_count;
	sc->ip->i_d.di_nblocks = rb->nblocks + delta;
	xfs_trans_log_inode(sc->tp, sc->ip, XFS_ILOG_CORE);

	/*
	 * Adjust the quota counts by the difference in size between the old
	 * and new bmbt.
	 */
	if (delta == 0 || !XFS_IS_QUOTA_ON(sc->mp))
		return 0;

	error = xrep_ino_dqattach(sc);
	if (error)
		return error;

	xfs_trans_mod_dquot_byino(sc->tp, sc->ip, XFS_TRANS_DQ_BCOUNT, delta);
	return 0;
}

/* Create a new iext tree and load it with block mappings. */
STATIC int
xrep_bmap_extents_load(
	struct xrep_bmap	*rb,
	struct xfs_btree_cur	*bmap_cur)
{
	struct xfs_iext_cursor	icur;
	struct xbtree_ifakeroot	*ifake = &rb->new_fork_info.ifake;
	struct xfs_ifork	*ifp = ifake->if_fork;
	unsigned int		i;
	int			error;

	ASSERT(ifp->if_bytes == 0);

	/* Add all the records to the incore extent tree. */
	rb->iter = 0;
	xfs_iext_first(ifp, &icur);
	for (i = 0; i < ifake->if_extents; i++) {
		error = xrep_bmap_get_data(bmap_cur, rb);
		if (error)
			return error;
		xfs_iext_insert_raw(ifp, &icur, &bmap_cur->bc_rec.b);
		xfs_iext_next(ifp, &icur);
	}
	ifp->if_flags = XFS_IFEXTENTS;

	return 0;
}

/* Reserve new btree blocks and bulk load all the bmap records. */
STATIC int
xrep_bmap_btree_load(
	struct xrep_bmap	*rb,
	struct xfs_btree_cur	**bmap_curp)
{
	struct xfs_btree_bload	bmap_bload = {
		.get_data	= xrep_bmap_get_data,
		.alloc_block	= xrep_bmap_alloc_block,
		.iroot_size	= xrep_bmap_iroot_size,
	};
	struct xfs_scrub	*sc = rb->sc;
	struct xbtree_ifakeroot	*ifake = &rb->new_fork_info.ifake;
	int			error;

	xrep_bload_estimate_slack(sc, &bmap_bload);

	/* Compute how many blocks we'll need. */
	error = xfs_btree_bload_compute_geometry(*bmap_curp, &bmap_bload,
			ifake->if_extents);
	if (error)
		return error;
	xfs_btree_del_cursor(*bmap_curp, error);
	*bmap_curp = NULL;

	/*
	 * Guess how many blocks we're going to need to rebuild an entire bmap
	 * from the number of extents we found, and pump up our transaction to
	 * have sufficient block reservation.
	 */
	error = xfs_trans_reserve_more(sc->tp, bmap_bload.nr_blocks, 0);
	if (error)
		return error;

	/*
	 * Reserve the space we'll need for the new btree.  Drop the cursor
	 * while we do this because that can roll the transaction and cursors
	 * can't handle that.
	 */
	error = xrep_newbt_reserve_space(&rb->new_fork_info,
			bmap_bload.nr_blocks);
	if (error)
		return error;

	/* Add all observed bmap records. */
	rb->iter = 0;
	*bmap_curp = xfs_bmbt_stage_cursor(sc->mp, sc->tp, sc->ip, ifake);
	return xfs_btree_bload(*bmap_curp, &bmap_bload, rb);
}

/*
 * Use the collected bmap information to stage a new bmap fork.  If this is
 * successful we'll return with the new fork information logged to the repair
 * transaction but not yet committed.
 */
STATIC int
xrep_bmap_build_new_fork(
	struct xrep_bmap	*rb)
{
	struct xfs_owner_info	oinfo;
	struct xfs_scrub	*sc = rb->sc;
	struct xfs_btree_cur	*bmap_cur;
	struct xbtree_ifakeroot	*ifake = &rb->new_fork_info.ifake;
	int			error;

	/*
	 * Sort the bmap extents by startblock to avoid btree splits when we
	 * rebuild the bmbt btree.
	 */
	error = xfbma_sort(rb->bmap_records, xrep_bmap_extent_cmp);
	if (error)
		return error;

	/*
	 * Prepare to construct the new fork by initializing the new btree
	 * structure and creating a fake ifork in the ifakeroot structure.
	 */
	xfs_rmap_ino_bmbt_owner(&oinfo, sc->ip->i_ino, rb->whichfork);
	xrep_newbt_init_inode(&rb->new_fork_info, sc, rb->whichfork, &oinfo);
	bmap_cur = xfs_bmbt_stage_cursor(sc->mp, sc->tp, sc->ip, ifake);

	/*
	 * Figure out the size and format of the new fork, then fill it with
	 * all the bmap records we've found.  Join the inode to the transaction
	 * so that we can roll the transaction while holding the inode locked.
	 */
	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	ifake->if_extents = xfbma_length(rb->bmap_records);
	if (XFS_BMDR_SPACE_CALC(ifake->if_extents) <=
	    XFS_DFORK_SIZE(&sc->ip->i_d, sc->mp, rb->whichfork)) {
		ifake->if_format = XFS_DINODE_FMT_EXTENTS;
		error = xrep_bmap_extents_load(rb, bmap_cur);
	} else {
		ifake->if_format = XFS_DINODE_FMT_BTREE;
		error = xrep_bmap_btree_load(rb, &bmap_cur);
	}
	if (error)
		goto err_cur;

	/*
	 * Install the new fork in the inode.  After this point the old mapping
	 * data are no longer accessible and the new tree is live.  We delete
	 * the cursor immediately after committing the staged root because the
	 * staged fork might be in extents format.
	 */
	xfs_bmbt_commit_staged_btree(bmap_cur, rb->whichfork);
	xfs_btree_del_cursor(bmap_cur, 0);

	/* Reset the inode counters now that we've changed the fork. */
	error = xrep_bmap_reset_counters(rb);
	if (error)
		goto err_newbt;

	/* Dispose of any unused blocks and the accounting information. */
	xrep_newbt_destroy(&rb->new_fork_info, error);

	return xfs_trans_roll_inode(&sc->tp, sc->ip);
err_cur:
	if (bmap_cur)
		xfs_btree_del_cursor(bmap_cur, error);
err_newbt:
	xrep_newbt_destroy(&rb->new_fork_info, error);
	return error;
}

/*
 * Now that we've logged the new inode btree, invalidate all of the old blocks
 * and free them, if there were any.
 */
STATIC int
xrep_bmap_remove_old_tree(
	struct xrep_bmap	*rb)
{
	struct xfs_scrub	*sc = rb->sc;
	struct xfs_owner_info	oinfo;

	/* Free the old bmbt blocks if they're not in use. */
	xfs_rmap_ino_bmbt_owner(&oinfo, sc->ip->i_ino, rb->whichfork);
	return xrep_reap_extents(sc, &rb->old_bmbt_blocks, &oinfo,
			XFS_AG_RESV_NONE);
}

/* Check for garbage inputs. */
STATIC int
xrep_bmap_check_inputs(
	struct xfs_scrub	*sc,
	int			whichfork)
{
	ASSERT(whichfork == XFS_DATA_FORK || whichfork == XFS_ATTR_FORK);

	/* Don't know how to repair the other fork formats. */
	if (XFS_IFORK_FORMAT(sc->ip, whichfork) != XFS_DINODE_FMT_EXTENTS &&
	    XFS_IFORK_FORMAT(sc->ip, whichfork) != XFS_DINODE_FMT_BTREE)
		return -EOPNOTSUPP;

	/*
	 * If there's no attr fork area in the inode, there's no attr fork to
	 * rebuild.
	 */
	if (whichfork == XFS_ATTR_FORK) {
		if (!XFS_IFORK_Q(sc->ip))
			return -ENOENT;
		return 0;
	}

	/* Only files, symlinks, and directories get to have data forks. */
	switch (VFS_I(sc->ip)->i_mode & S_IFMT) {
	case S_IFREG:
	case S_IFDIR:
	case S_IFLNK:
		/* ok */
		break;
	default:
		return -EINVAL;
	}

	/* If we somehow have delalloc extents, forget it. */
	if (sc->ip->i_delayed_blks)
		return -EBUSY;

	/* Don't know how to rebuild realtime data forks. */
	if (XFS_IS_REALTIME_INODE(sc->ip))
		return -EOPNOTSUPP;

	return 0;
}

/* Repair an inode fork. */
STATIC int
xrep_bmap(
	struct xfs_scrub	*sc,
	int			whichfork)
{
	struct xrep_bmap	*rb;
	int			error = 0;

	error = xrep_bmap_check_inputs(sc, whichfork);
	if (error)
		return error;

	rb = kmem_zalloc(sizeof(struct xrep_bmap), KM_NOFS | KM_MAYFAIL);
	if (!rb)
		return -ENOMEM;
	rb->sc = sc;
	rb->whichfork = whichfork;

	/* Set up some storage */
	rb->bmap_records = xfbma_init(sizeof(struct xfs_bmbt_rec));
	if (IS_ERR(rb->bmap_records)) {
		error = PTR_ERR(rb->bmap_records);
		goto out_rb;
	}

	/* Collect all reverse mappings for this fork's extents. */
	xbitmap_init(&rb->old_bmbt_blocks);
	error = xrep_bmap_find_mappings(rb);
	if (error)
		goto out_bitmap;

	/* Rebuild the bmap information. */
	error = xrep_bmap_build_new_fork(rb);
	if (error)
		goto out_bitmap;

	/* Kill the old tree. */
	error = xrep_bmap_remove_old_tree(rb);

out_bitmap:
	xbitmap_destroy(&rb->old_bmbt_blocks);
	xfbma_destroy(rb->bmap_records);
out_rb:
	kmem_free(rb);
	return error;
}

/* Repair an inode's data fork. */
int
xrep_bmap_data(
	struct xfs_scrub	*sc)
{
	return xrep_bmap(sc, XFS_DATA_FORK);
}

/* Repair an inode's attr fork. */
int
xrep_bmap_attr(
	struct xfs_scrub	*sc)
{
	return xrep_bmap(sc, XFS_ATTR_FORK);
}
