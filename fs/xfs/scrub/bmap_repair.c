// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
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
#include "xfs_ialloc.h"
#include "xfs_ag.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/bitmap.h"
#include "scrub/xfarray.h"

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
	struct xfs_btree_bload	bmap_bload;

	/* List of new bmap records. */
	struct xfarray		*bmap_records;

	struct xfs_scrub	*sc;

	/* How many blocks did we find allocated to this file? */
	xfs_rfsblock_t		nblocks;

	/* How many bmbt blocks did we find for this fork? */
	xfs_rfsblock_t		old_bmbt_block_count;

	/* get_record()'s position in the free space record array. */
	uint64_t		iter;

	/* Which fork are we fixing? */
	int			whichfork;

	/* Do we allow unwritten extents? */
	bool			allow_unwritten;
};

/* Remember this reverse-mapping as a series of bmap records. */
STATIC int
xrep_bmap_from_rmap(
	struct xrep_bmap	*rb,
	xfs_fileoff_t		startoff,
	xfs_fsblock_t		startblock,
	xfs_filblks_t		blockcount,
	bool			unwritten)
{
	struct xfs_bmbt_rec	rbe;
	struct xfs_bmbt_irec	irec;
	int			error = 0;

	irec.br_startoff = startoff;
	irec.br_startblock = startblock;
	irec.br_state = unwritten ? XFS_EXT_UNWRITTEN : XFS_EXT_NORM;

	do {
		irec.br_blockcount = min_t(xfs_filblks_t, blockcount,
				MAXEXTLEN);
		xfs_bmbt_disk_set_all(&rbe, &irec);

		trace_xrep_bmap_found(rb->sc->ip, rb->whichfork, &irec);

		if (xchk_should_terminate(rb->sc, &error))
			return error;

		error = xfarray_append(rb->bmap_records, &rbe);
		if (error)
			return error;

		irec.br_startblock += irec.br_blockcount;
		irec.br_startoff += irec.br_blockcount;
		blockcount -= irec.br_blockcount;
	} while (blockcount > 0);

	return 0;
}

/* Check for any obvious errors or conflicts in the file mapping. */
STATIC int
xrep_bmap_check_fork_rmap(
	struct xrep_bmap		*rb,
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec)
{
	struct xfs_scrub		*sc = rb->sc;
	bool				is_freesp, has_inodes;
	int				error;

	/*
	 * Data extents for rt files are never stored on the data device, but
	 * everything else (xattrs, bmbt blocks) can be.
	 */
	if (XFS_IS_REALTIME_INODE(sc->ip) &&
	    !(rec->rm_flags & (XFS_RMAP_ATTR_FORK | XFS_RMAP_BMBT_BLOCK)))
		return -EFSCORRUPTED;

	/* Check that this is within the AG. */
	if (!xfs_verify_agbext(sc->mp, cur->bc_ag.pag->pag_agno,
				rec->rm_startblock, rec->rm_blockcount))
		return -EFSCORRUPTED;

	/* Check the file offset range. */
	if (!(rec->rm_flags & XFS_RMAP_BMBT_BLOCK) &&
	    !xfs_verify_fileext(sc->mp, rec->rm_offset, rec->rm_blockcount))
		return -EFSCORRUPTED;

	/* No contradictory flags. */
	if ((rec->rm_flags & (XFS_RMAP_ATTR_FORK | XFS_RMAP_BMBT_BLOCK)) &&
	    (rec->rm_flags & XFS_RMAP_UNWRITTEN))
		return -EFSCORRUPTED;

	/* Make sure this isn't free space. */
	error = xfs_alloc_has_record(sc->sa.bno_cur, rec->rm_startblock,
			rec->rm_blockcount, &is_freesp);
	if (error)
		return error;
	if (is_freesp)
		return -EFSCORRUPTED;

	/* Must not be an inode chunk. */
	error = xfs_ialloc_has_inodes_at_extent(sc->sa.ino_cur,
			rec->rm_startblock, rec->rm_blockcount, &has_inodes);
	if (error)
		return error;
	if (has_inodes)
		return -EFSCORRUPTED;

	return 0;
}

/* Record extents that belong to this inode's fork. */
STATIC int
xrep_bmap_walk_rmap(
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec,
	void				*priv)
{
	struct xrep_bmap		*rb = priv;
	struct xfs_mount		*mp = cur->bc_mp;
	xfs_fsblock_t			fsbno;
	int				error = 0;

	if (xchk_should_terminate(rb->sc, &error))
		return error;

	if (rec->rm_owner != rb->sc->ip->i_ino)
		return 0;

	error = xrep_bmap_check_fork_rmap(rb, cur, rec);
	if (error)
		return error;

	/*
	 * Record all blocks allocated to this file even if the extent isn't
	 * for the fork we're rebuilding so that we can reset di_nblocks later.
	 */
	rb->nblocks += rec->rm_blockcount;

	/* If this rmap isn't for the fork we want, we're done. */
	if (rb->whichfork == XFS_DATA_FORK &&
	    (rec->rm_flags & XFS_RMAP_ATTR_FORK))
		return 0;
	if (rb->whichfork == XFS_ATTR_FORK &&
	    !(rec->rm_flags & XFS_RMAP_ATTR_FORK))
		return 0;

	/* Reject unwritten extents if we don't allow those. */
	if ((rec->rm_flags & XFS_RMAP_UNWRITTEN) && !rb->allow_unwritten)
		return -EFSCORRUPTED;

	fsbno = XFS_AGB_TO_FSB(mp, cur->bc_ag.pag->pag_agno,
			rec->rm_startblock);

	if (rec->rm_flags & XFS_RMAP_BMBT_BLOCK) {
		rb->old_bmbt_block_count += rec->rm_blockcount;
		return xbitmap_set(&rb->old_bmbt_blocks, fsbno,
				rec->rm_blockcount);
	}

	return xrep_bmap_from_rmap(rb, rec->rm_offset, fsbno,
			rec->rm_blockcount,
			rec->rm_flags & XFS_RMAP_UNWRITTEN);
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
	struct xfs_perag	*pag)
{
	struct xfs_scrub	*sc = rb->sc;
	int			error;

	error = xrep_ag_init(sc, pag, &sc->sa);
	if (error)
		return error;

	error = xfs_rmap_query_all(sc->sa.rmap_cur, xrep_bmap_walk_rmap, rb);
	xchk_ag_free(sc, &sc->sa);
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
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno;
	int			error = 0;

	/* Iterate the rmaps for extents. */
	for_each_perag(sc->mp, agno, pag) {
		error = xrep_bmap_scan_ag(rb, pag);
		if (error) {
			xfs_perag_put(pag);
			return error;
		}
	}

	return 0;
}

/* Retrieve bmap data for bulk load. */
STATIC int
xrep_bmap_get_record(
	struct xfs_btree_cur	*cur,
	void			*priv)
{
	struct xfs_bmbt_rec	rec;
	struct xfs_bmbt_irec	*irec = &cur->bc_rec.b;
	struct xrep_bmap	*rb = priv;
	int			error;

	error = xfarray_load_next(rb->bmap_records, &rb->iter, &rec);
	if (error)
		return error;

	xfs_bmbt_disk_get_all(&rec, irec);
	return 0;
}

/* Feed one of the new btree blocks to the bulk loader. */
STATIC int
xrep_bmap_claim_block(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	void			*priv)
{
	struct xrep_bmap        *rb = priv;
	int			error;

	error = xrep_newbt_relog_efis(&rb->new_fork_info);
	if (error)
		return error;

	return xrep_newbt_claim_block(cur, &rb->new_fork_info, ptr);
}

/* Figure out how much space we need to create the incore btree root block. */
STATIC size_t
xrep_bmap_iroot_size(
	struct xfs_btree_cur	*cur,
	unsigned int		level,
	unsigned int		nr_this_level,
	void			*priv)
{
	ASSERT(level > 0);

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

	/*
	 * Update the inode block counts to reflect the extents we found in the
	 * rmapbt.
	 */
	delta = ifake->if_blocks - rb->old_bmbt_block_count;
	sc->ip->i_nblocks = rb->nblocks + delta;
	xfs_trans_log_inode(sc->tp, sc->ip, XFS_ILOG_CORE);

	/*
	 * Adjust the quota counts by the difference in size between the old
	 * and new bmbt.
	 */
	xfs_trans_mod_dquot_byino(sc->tp, sc->ip, XFS_TRANS_DQ_BCOUNT, delta);
	return 0;
}

/*
 * Create a new iext tree and load it with block mappings.  If the inode is
 * in extents format, that's all we need to do to commit the new mappings.
 * If it is in btree format, this takes care of preloading the incore tree.
 */
STATIC int
xrep_bmap_extents_load(
	struct xrep_bmap	*rb,
	struct xfs_btree_cur	*bmap_cur,
	uint64_t		nextents)
{
	struct xfs_iext_cursor	icur, old_icur;
	struct xfs_bmbt_irec	irec;
	struct xbtree_ifakeroot	*ifake = &rb->new_fork_info.ifake;
	struct xfs_ifork	*ifp = ifake->if_fork;
	struct xfs_ifork	*old_ifp = XFS_IFORK_PTR(rb->sc->ip,
							 rb->whichfork);
	unsigned int		i;
	int			error;

	ASSERT(ifp->if_bytes == 0);

	/* Add all the records to the incore extent tree. */
	rb->iter = 0;
	xfs_iext_first(ifp, &icur);
	for (i = 0; i < nextents; i++) {
		error = xrep_bmap_get_record(bmap_cur, rb);
		if (error)
			return error;
		xfs_iext_insert_raw(ifp, &icur, &bmap_cur->bc_rec.b);
		ifp->if_nextents++;
		xfs_iext_next(ifp, &icur);
	}

	/* Now add the delalloc extents from the old incore extent tree. */
	xfs_iext_first(old_ifp, &old_icur);
	while (xfs_iext_get_extent(old_ifp, &old_icur, &irec)) {
		if (isnullstartblock(irec.br_startblock)) {
			xfs_iext_insert_raw(ifp, &icur, &irec);
			xfs_iext_next(ifp, &icur);
		}
		xfs_iext_next(old_ifp, &old_icur);
	}

	return 0;
}

/*
 * Reserve new btree blocks, bulk load the bmap records into the ondisk btree,
 * and load the incore extent tree.
 */
STATIC int
xrep_bmap_btree_load(
	struct xrep_bmap	*rb,
	struct xfs_btree_cur	*bmap_cur,
	uint64_t		nextents)
{
	struct xfs_scrub	*sc = rb->sc;
	int			error;

	rb->bmap_bload.get_record = xrep_bmap_get_record;
	rb->bmap_bload.claim_block = xrep_bmap_claim_block;
	rb->bmap_bload.iroot_size = xrep_bmap_iroot_size;
	xrep_bload_estimate_slack(sc, &rb->bmap_bload);

	/* Compute how many blocks we'll need. */
	error = xfs_btree_bload_compute_geometry(bmap_cur, &rb->bmap_bload,
			nextents);
	if (error)
		return error;

	/* Last chance to abort before we start committing fixes. */
	if (xchk_should_terminate(sc, &error))
		return error;

	/*
	 * Guess how many blocks we're going to need to rebuild an entire bmap
	 * from the number of extents we found, and pump up our transaction to
	 * have sufficient block reservation.
	 */
	error = xfs_trans_reserve_more_inode(sc->tp, sc->ip,
			rb->bmap_bload.nr_blocks, 0);
	if (error)
		return error;

	/* Reserve the space we'll need for the new btree. */
	error = xrep_newbt_alloc_blocks(&rb->new_fork_info,
			rb->bmap_bload.nr_blocks);
	if (error)
		return error;

	/* Add all observed bmap records. */
	rb->iter = 0;
	error = xfs_btree_bload(bmap_cur, &rb->bmap_bload, rb);
	if (error)
		return error;

	/*
	 * Load the new bmap records into the new incore extent tree to
	 * preserve delalloc reservations for regular files.  The directory
	 * code loads the extent tree during xfs_dir_open and assumes
	 * thereafter that it remains loaded, so we must not violate that
	 * assumption.
	 */
	return xrep_bmap_extents_load(rb, bmap_cur, nextents);
}

/*
 * Use the collected bmap information to stage a new bmap fork.  If this is
 * successful we'll return with the new fork information logged to the repair
 * transaction but not yet committed.  The caller must ensure that the inode
 * is joined to the transaction; the inode will be joined to a clean
 * transaction when the function returns.
 */
STATIC int
xrep_bmap_build_new_fork(
	struct xrep_bmap	*rb)
{
	struct xfs_owner_info	oinfo;
	struct xfs_scrub	*sc = rb->sc;
	struct xfs_btree_cur	*bmap_cur;
	struct xbtree_ifakeroot	*ifake = &rb->new_fork_info.ifake;
	uint64_t		nextents;
	int			error;

	/*
	 * Sort the bmap extents by startblock to avoid btree splits when we
	 * rebuild the bmbt btree.
	 */
	error = xfarray_sort(rb->bmap_records, xrep_bmap_extent_cmp);
	if (error)
		return error;

	/*
	 * Prepare to construct the new fork by initializing the new btree
	 * structure and creating a fake ifork in the ifakeroot structure.
	 */
	xfs_rmap_ino_bmbt_owner(&oinfo, sc->ip->i_ino, rb->whichfork);
	xrep_newbt_init_inode(&rb->new_fork_info, sc, rb->whichfork, &oinfo);
	bmap_cur = xfs_bmbt_stage_cursor(sc->mp, sc->ip, ifake);

	/*
	 * Figure out the size and format of the new fork, then fill it with
	 * all the bmap records we've found.  Join the inode to the transaction
	 * so that we can roll the transaction while holding the inode locked.
	 */
	nextents = xfarray_length(rb->bmap_records);
	if (nextents <= XFS_IFORK_MAXEXT(sc->ip, rb->whichfork)) {
		ifake->if_fork->if_format = XFS_DINODE_FMT_EXTENTS;
		error = xrep_bmap_extents_load(rb, bmap_cur, nextents);
	} else {
		ifake->if_fork->if_format = XFS_DINODE_FMT_BTREE;
		error = xrep_bmap_btree_load(rb, bmap_cur, nextents);
	}
	if (error)
		goto err_cur;

	/*
	 * Install the new fork in the inode.  After this point the old mapping
	 * data are no longer accessible and the new tree is live.  We delete
	 * the cursor immediately after committing the staged root because the
	 * staged fork might be in extents format.
	 */
	xfs_bmbt_commit_staged_btree(bmap_cur, sc->tp, rb->whichfork);
	xfs_btree_del_cursor(bmap_cur, 0);

	/* Reset the inode counters now that we've changed the fork. */
	error = xrep_bmap_reset_counters(rb);
	if (error)
		goto err_newbt;

	/* Dispose of any unused blocks and the accounting information. */
	xrep_newbt_destroy(&rb->new_fork_info, error);
	return xrep_roll_trans(sc);

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

/* Check for garbage inputs.  Returns -ECANCELED if there's nothing to do. */
STATIC int
xrep_bmap_check_inputs(
	struct xfs_scrub	*sc,
	int			whichfork)
{
	struct xfs_ifork	*ifp = XFS_IFORK_PTR(sc->ip, whichfork);

	ASSERT(whichfork == XFS_DATA_FORK || whichfork == XFS_ATTR_FORK);

	if (!xfs_has_rmapbt(sc->mp))
		return -EOPNOTSUPP;

	/* No fork means nothing to rebuild. */
	if (!ifp)
		return -ECANCELED;

	/*
	 * We only know how to repair extent mappings, which is to say that we
	 * only support extents and btree fork format.  Repairs to a local
	 * format fork require a higher level repair function, so we do not
	 * have any work to do here.
	 */
	switch (ifp->if_format) {
	case XFS_DINODE_FMT_DEV:
	case XFS_DINODE_FMT_LOCAL:
	case XFS_DINODE_FMT_UUID:
		return -ECANCELED;
	case XFS_DINODE_FMT_EXTENTS:
	case XFS_DINODE_FMT_BTREE:
		break;
	default:
		return -EFSCORRUPTED;
	}

	if (whichfork == XFS_ATTR_FORK)
		return 0;

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

	/* Don't know how to rebuild realtime data forks. */
	if (XFS_IS_REALTIME_INODE(sc->ip))
		return -EOPNOTSUPP;

	return 0;
}

/* Repair an inode fork. */
int
xrep_bmap(
	struct xfs_scrub	*sc,
	int			whichfork,
	bool			allow_unwritten)
{
	struct xrep_bmap	*rb;
	int			error = 0;

	error = xrep_bmap_check_inputs(sc, whichfork);
	if (error == -ECANCELED)
		return 0;
	if (error)
		return error;

	rb = kmem_zalloc(sizeof(struct xrep_bmap), KM_NOFS | KM_MAYFAIL);
	if (!rb)
		return -ENOMEM;
	rb->sc = sc;
	rb->whichfork = whichfork;
	rb->allow_unwritten = allow_unwritten;

	/* Set up some storage */
	error = xfarray_create(sc->mp, "bmap records",
			sizeof(struct xfs_bmbt_rec), &rb->bmap_records);
	if (error)
		goto out_rb;

	/* Collect all reverse mappings for this fork's extents. */
	xbitmap_init(&rb->old_bmbt_blocks);
	error = xrep_bmap_find_mappings(rb);
	if (error)
		goto out_bitmap;

	/* Attach quotas and inode to transaction. */
	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	error = xrep_ino_dqattach(sc);
	if (error)
		return error;

	/* Rebuild the bmap information. */
	error = xrep_bmap_build_new_fork(rb);
	if (error)
		goto out_bitmap;

	/* Kill the old tree. */
	error = xrep_bmap_remove_old_tree(rb);

out_bitmap:
	xbitmap_destroy(&rb->old_bmbt_blocks);
	xfarray_destroy(rb->bmap_records);
out_rb:
	kmem_free(rb);
	return error;
}

/* Repair an inode's data fork. */
int
xrep_bmap_data(
	struct xfs_scrub	*sc)
{
	return xrep_bmap(sc, XFS_DATA_FORK, true);
}

/* Repair an inode's attr fork. */
int
xrep_bmap_attr(
	struct xfs_scrub	*sc)
{
	return xrep_bmap(sc, XFS_ATTR_FORK, false);
}
