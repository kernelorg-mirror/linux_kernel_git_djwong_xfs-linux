// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017 Oracle.  All Rights Reserved.
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
#include "xfs_alloc.h"
#include "xfs_ialloc.h"
#include "xfs_ialloc_btree.h"
#include "xfs_icache.h"
#include "xfs_rmap.h"
#include "xfs_log.h"
#include "xfs_trans_priv.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"

/*
 * Set us up to scrub inode btrees.
 * If we detect a discrepancy between the inobt and the inode,
 * try again after forcing logged inode cores out to disk.
 */
int
xchk_setup_ag_iallocbt(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	return xchk_setup_ag_btree(sc, ip, sc->try_harder);
}

/* Inode btree scrubber. */

struct xchk_iallocbt {
	/* owner info for inode blocks */
	struct xfs_owner_info	oinfo;

	/* Number of inodes we see while scanning inobt. */
	unsigned long long	inodes;

	/* Blocks and inodes per inode cluster. */
	unsigned int		blocks_per_cluster;
	unsigned int		inodes_per_cluster;

	/* Block alignment of inode clusters. */
	unsigned int		cluster_align;
};

/*
 * If we're checking the finobt, cross-reference with the inobt.
 * Otherwise we're checking the inobt; if there is an finobt, make sure
 * we have a record or not depending on freecount.
 */
static inline void
xchk_iallocbt_chunk_xref_other(
	struct xfs_scrub		*sc,
	struct xfs_inobt_rec_incore	*irec,
	xfs_agino_t			agino)
{
	struct xfs_btree_cur		**pcur;
	bool				has_irec;
	int				error;

	if (sc->sm->sm_type == XFS_SCRUB_TYPE_FINOBT)
		pcur = &sc->sa.ino_cur;
	else
		pcur = &sc->sa.fino_cur;
	if (!(*pcur))
		return;
	error = xfs_ialloc_has_inode_record(*pcur, agino, agino, &has_irec);
	if (!xchk_should_check_xref(sc, &error, pcur))
		return;
	if (((irec->ir_freecount > 0 && !has_irec) ||
	     (irec->ir_freecount == 0 && has_irec)))
		xchk_btree_xref_set_corrupt(sc, *pcur, 0);
}

/* Cross-reference with the other btrees. */
STATIC void
xchk_iallocbt_chunk_xref(
	struct xfs_scrub		*sc,
	struct xfs_inobt_rec_incore	*irec,
	xfs_agino_t			agino,
	xfs_agblock_t			agbno,
	xfs_extlen_t			len)
{
	struct xfs_owner_info		oinfo;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return;

	xchk_xref_is_used_space(sc, agbno, len);
	xchk_iallocbt_chunk_xref_other(sc, irec, agino);
	xfs_rmap_ag_owner(&oinfo, XFS_RMAP_OWN_INODES);
	xchk_xref_is_owned_by(sc, agbno, len, &oinfo);
	xchk_xref_is_not_shared(sc, agbno, len);
}

/* Is this chunk worth checking? */
STATIC bool
xchk_iallocbt_chunk(
	struct xchk_btree		*bs,
	struct xfs_inobt_rec_incore	*irec,
	xfs_agino_t			agino,
	xfs_extlen_t			len)
{
	struct xfs_mount		*mp = bs->cur->bc_mp;
	xfs_agnumber_t			agno = bs->cur->bc_private.a.agno;
	xfs_agblock_t			bno;

	bno = XFS_AGINO_TO_AGBNO(mp, agino);
	if (bno + len <= bno ||
	    !xfs_verify_agbno(mp, agno, bno) ||
	    !xfs_verify_agbno(mp, agno, bno + len - 1))
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	xchk_iallocbt_chunk_xref(bs->sc, irec, agino, bno, len);

	return true;
}

/* Count the number of free inodes. */
static unsigned int
xchk_iallocbt_freecount(
	xfs_inofree_t			freemask)
{
	BUILD_BUG_ON(sizeof(freemask) != sizeof(__u64));
	return hweight64(freemask);
}

/*
 * Given the number of an inode within an inode cluster, check that the inode's
 * allocation status matches ir_free in the inobt record.
 *
 * @chunk_ioff is the inode offset of the cluster within the @irec.
 * @irec is the inobt record.
 * @bp is the cluster buffer.
 * @loop_ioff is the inode offset within the inode cluster.
 */
STATIC int
xchk_iallocbt_check_cluster_ifree(
	struct xchk_btree		*bs,
	struct xfs_inobt_rec_incore	*irec,
	unsigned int			chunk_ioff,
	struct xfs_buf			*bp,
	unsigned int			loop_ioff)
{
	struct xfs_mount		*mp = bs->cur->bc_mp;
	struct xfs_dinode		*dip;
	xfs_ino_t			fsino;
	xfs_agino_t			agino;
	unsigned int			offset;
	bool				irec_free;
	bool				ino_inuse;
	bool				freemask_ok;
	int				error;

	if (xchk_should_terminate(bs->sc, &error))
		return error;

	/*
	 * Given an inobt record, an offset of a cluster within the record,
	 * and an offset of an inode within a cluster, compute which fs inode
	 * we're talking about and the offset of the inode record within the
	 * inode buffer.
	 */
	agino = irec->ir_startino + chunk_ioff + loop_ioff;
	fsino = XFS_AGINO_TO_INO(mp, bs->cur->bc_private.a.agno, agino);
	offset = loop_ioff * mp->m_sb.sb_inodesize;
	dip = xfs_buf_offset(bp, offset);
	irec_free = (irec->ir_free & XFS_INOBT_MASK(chunk_ioff + loop_ioff));

	if (be16_to_cpu(dip->di_magic) != XFS_DINODE_MAGIC ||
	    (dip->di_version >= 3 && be64_to_cpu(dip->di_ino) != fsino)) {
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);
		goto out;
	}

	error = xfs_icache_inode_is_allocated(mp, bs->cur->bc_tp, fsino,
			&ino_inuse);
	if (error == -ENODATA) {
		/* Not cached, just read the disk buffer */
		freemask_ok = irec_free ^ !!(dip->di_mode);
		if (!bs->sc->try_harder && !freemask_ok)
			return -EDEADLOCK;
	} else if (error < 0) {
		/*
		 * Inode is only half assembled, or there was an IO error,
		 * or the verifier failed, so don't bother trying to check.
		 * The inode scrubber can deal with this.
		 */
		goto out;
	} else {
		/* Inode is all there. */
		freemask_ok = irec_free ^ ino_inuse;
	}
	if (!freemask_ok)
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);
out:
	return 0;
}

/*
 * Check that the holemask and freemask of a hypothetical inode cluster match
 * what's actually on disk.  If sparse inodes are enabled, the cluster does
 * not actually have to map to inodes if the corresponding holemask bit is set.
 *
 * @chunk_ioff is the inode offset of the cluster within the @irec.
 */
STATIC int
xchk_iallocbt_check_cluster(
	struct xchk_btree		*bs,
	struct xchk_iallocbt		*iabt,
	struct xfs_inobt_rec_incore	*irec,
	unsigned int			chunk_ioff)
{
	struct xfs_imap			imap;
	struct xfs_mount		*mp = bs->cur->bc_mp;
	struct xfs_dinode		*dip;
	struct xfs_buf			*bp;
	unsigned int			nr_inodes;
	xfs_agnumber_t			agno = bs->cur->bc_private.a.agno;
	xfs_agblock_t			agbno;
	unsigned int			loop_ioff;
	uint16_t			cluster_mask = 0;
	uint16_t			ir_holemask;
	int				error = 0;

	nr_inodes = min_t(unsigned int, iabt->inodes_per_cluster,
			XFS_INODES_PER_CHUNK);

	/* Map this inode cluster */
	agbno = XFS_AGINO_TO_AGBNO(mp, irec->ir_startino + chunk_ioff);

	/* Compute a bitmask for this cluster that can be used for holemask. */
	for (loop_ioff = 0;
	     loop_ioff < nr_inodes;
	     loop_ioff += XFS_INODES_PER_HOLEMASK_BIT)
		cluster_mask |= XFS_INOBT_MASK((chunk_ioff + loop_ioff) /
				XFS_INODES_PER_HOLEMASK_BIT);

	ir_holemask = (irec->ir_holemask & cluster_mask);
	imap.im_blkno = XFS_AGB_TO_DADDR(mp, agno, agbno);
	imap.im_len = XFS_FSB_TO_BB(mp, iabt->blocks_per_cluster);
	imap.im_boffset = 0;

	trace_xchk_iallocbt_check_cluster(mp, agno, irec->ir_startino,
			imap.im_blkno, imap.im_len, chunk_ioff, nr_inodes,
			cluster_mask, ir_holemask,
			XFS_INO_TO_OFFSET(mp, irec->ir_startino + chunk_ioff));

	/* The whole cluster must be a hole or not a hole. */
	if (ir_holemask != cluster_mask && ir_holemask != 0) {
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);
		return 0;
	}

	/* If any part of this is a hole, skip it. */
	if (ir_holemask) {
		xchk_xref_is_not_owned_by(bs->sc, agbno,
				iabt->blocks_per_cluster, &iabt->oinfo);
		return 0;
	}

	xchk_xref_is_owned_by(bs->sc, agbno, iabt->blocks_per_cluster,
			&iabt->oinfo);

	/* Grab the inode cluster buffer. */
	error = xfs_imap_to_bp(mp, bs->cur->bc_tp, &imap, &dip, &bp, 0, 0);
	if (!xchk_btree_xref_process_error(bs->sc, bs->cur, 0, &error))
		return error;

	/* Check free status of each inode within this cluster. */
	for (loop_ioff = 0; loop_ioff < nr_inodes; loop_ioff++) {
		error = xchk_iallocbt_check_cluster_ifree(bs, irec, chunk_ioff,
				bp, loop_ioff);
		if (error)
			break;
	}

	xfs_trans_brelse(bs->cur->bc_tp, bp);
	return error;
}

/* Make sure the free mask is consistent with what the inodes think. */
STATIC int
xchk_iallocbt_check_freemask(
	struct xchk_btree		*bs,
	struct xchk_iallocbt		*iabt,
	struct xfs_inobt_rec_incore	*irec)
{
	unsigned int			chunk_ioff;
	int				error = 0;

	for (chunk_ioff = 0;
	     chunk_ioff < XFS_INODES_PER_CHUNK;
	     chunk_ioff += iabt->inodes_per_cluster) {
		error = xchk_iallocbt_check_cluster(bs, iabt, irec, chunk_ioff);
		if (error)
			break;
	}

	return error;
}

/* Scrub an inobt/finobt record. */
STATIC int
xchk_iallocbt_rec(
	struct xchk_btree		*bs,
	union xfs_btree_rec		*rec)
{
	struct xfs_mount		*mp = bs->cur->bc_mp;
	struct xchk_iallocbt		*iabt = bs->private;
	struct xfs_inobt_rec_incore	irec;
	uint64_t			holes;
	xfs_agnumber_t			agno = bs->cur->bc_private.a.agno;
	xfs_agino_t			agino;
	xfs_agblock_t			agbno;
	xfs_extlen_t			len;
	int				holecount;
	int				i;
	int				error = 0;
	unsigned int			real_freecount;
	uint16_t			holemask;

	xfs_inobt_btrec_to_irec(mp, rec, &irec);

	if (irec.ir_count > XFS_INODES_PER_CHUNK ||
	    irec.ir_freecount > XFS_INODES_PER_CHUNK)
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	real_freecount = irec.ir_freecount +
			(XFS_INODES_PER_CHUNK - irec.ir_count);
	if (real_freecount != xchk_iallocbt_freecount(irec.ir_free))
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	agino = irec.ir_startino;
	/* Record has to be properly aligned within the AG. */
	if (!xfs_verify_agino(mp, agno, agino) ||
	    !xfs_verify_agino(mp, agno, agino + XFS_INODES_PER_CHUNK - 1)) {
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);
		goto out;
	}

	/* Make sure this record is aligned to cluster and inoalignmnt size. */
	agbno = XFS_AGINO_TO_AGBNO(mp, irec.ir_startino);
	if ((agbno & (iabt->cluster_align - 1)) ||
	    (agbno & (iabt->blocks_per_cluster - 1)))
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	iabt->inodes += irec.ir_count;

	/* Handle non-sparse inodes */
	if (!xfs_inobt_issparse(irec.ir_holemask)) {
		len = XFS_B_TO_FSB(mp,
				XFS_INODES_PER_CHUNK * mp->m_sb.sb_inodesize);
		if (irec.ir_count != XFS_INODES_PER_CHUNK)
			xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

		if (!xchk_iallocbt_chunk(bs, &irec, agino, len))
			goto out;
		goto check_freemask;
	}

	/* Check each chunk of a sparse inode cluster. */
	holemask = irec.ir_holemask;
	holecount = 0;
	len = XFS_B_TO_FSB(mp,
			XFS_INODES_PER_HOLEMASK_BIT * mp->m_sb.sb_inodesize);
	holes = ~xfs_inobt_irec_to_allocmask(&irec);
	if ((holes & irec.ir_free) != holes ||
	    irec.ir_freecount > irec.ir_count)
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

	for (i = 0; i < XFS_INOBT_HOLEMASK_BITS; i++) {
		if (holemask & 1)
			holecount += XFS_INODES_PER_HOLEMASK_BIT;
		else if (!xchk_iallocbt_chunk(bs, &irec, agino, len))
			break;
		holemask >>= 1;
		agino += XFS_INODES_PER_HOLEMASK_BIT;
	}

	if (holecount > XFS_INODES_PER_CHUNK ||
	    holecount + irec.ir_count != XFS_INODES_PER_CHUNK)
		xchk_btree_set_corrupt(bs->sc, bs->cur, 0);

check_freemask:
	error = xchk_iallocbt_check_freemask(bs, iabt, &irec);
	if (error)
		goto out;

out:
	return error;
}

/*
 * Make sure the inode btrees are as large as the rmap thinks they are.
 * Don't bother if we're missing btree cursors, as we're already corrupt.
 */
STATIC void
xchk_iallocbt_xref_rmap_btreeblks(
	struct xfs_scrub	*sc,
	int			which)
{
	struct xfs_owner_info	oinfo;
	xfs_filblks_t		blocks;
	xfs_extlen_t		inobt_blocks = 0;
	xfs_extlen_t		finobt_blocks = 0;
	int			error;

	if (!sc->sa.ino_cur || !sc->sa.rmap_cur ||
	    (xfs_sb_version_hasfinobt(&sc->mp->m_sb) && !sc->sa.fino_cur) ||
	    xchk_skip_xref(sc->sm))
		return;

	/* Check that we saw as many inobt blocks as the rmap says. */
	error = xfs_btree_count_blocks(sc->sa.ino_cur, &inobt_blocks);
	if (!xchk_process_error(sc, 0, 0, &error))
		return;

	if (sc->sa.fino_cur) {
		error = xfs_btree_count_blocks(sc->sa.fino_cur, &finobt_blocks);
		if (!xchk_process_error(sc, 0, 0, &error))
			return;
	}

	xfs_rmap_ag_owner(&oinfo, XFS_RMAP_OWN_INOBT);
	error = xchk_count_rmap_ownedby_ag(sc, sc->sa.rmap_cur, &oinfo,
			&blocks);
	if (!xchk_should_check_xref(sc, &error, &sc->sa.rmap_cur))
		return;
	if (blocks != inobt_blocks + finobt_blocks)
		xchk_btree_set_corrupt(sc, sc->sa.ino_cur, 0);
}

/*
 * Make sure that the inobt records point to the same number of blocks as
 * the rmap says are owned by inodes.
 */
STATIC void
xchk_iallocbt_xref_rmap_inodes(
	struct xfs_scrub	*sc,
	int			which,
	unsigned long long	inodes)
{
	struct xfs_owner_info	oinfo;
	xfs_filblks_t		blocks;
	xfs_filblks_t		inode_blocks;
	int			error;

	if (!sc->sa.rmap_cur || xchk_skip_xref(sc->sm))
		return;

	/* Check that we saw as many inode blocks as the rmap knows about. */
	xfs_rmap_ag_owner(&oinfo, XFS_RMAP_OWN_INODES);
	error = xchk_count_rmap_ownedby_ag(sc, sc->sa.rmap_cur, &oinfo,
			&blocks);
	if (!xchk_should_check_xref(sc, &error, &sc->sa.rmap_cur))
		return;
	inode_blocks = XFS_B_TO_FSB(sc->mp, inodes * sc->mp->m_sb.sb_inodesize);
	if (blocks != inode_blocks)
		xchk_btree_xref_set_corrupt(sc, sc->sa.rmap_cur, 0);
}

/* Scrub the inode btrees for some AG. */
STATIC int
xchk_iallocbt(
	struct xfs_scrub	*sc,
	xfs_btnum_t		which)
{
	struct xfs_btree_cur	*cur;
	struct xfs_owner_info	oinfo;
	struct xchk_iallocbt	iabt = {
		.inodes		= 0,
		.cluster_align	= xfs_ialloc_cluster_alignment(sc->mp),
		.blocks_per_cluster = xfs_icluster_size_fsb(sc->mp),
	};
	int			error;

	xfs_rmap_ag_owner(&iabt.oinfo, XFS_RMAP_OWN_INODES);
	iabt.inodes_per_cluster = XFS_OFFBNO_TO_AGINO(sc->mp,
			iabt.blocks_per_cluster, 0);

	xfs_rmap_ag_owner(&oinfo, XFS_RMAP_OWN_INOBT);
	cur = which == XFS_BTNUM_INO ? sc->sa.ino_cur : sc->sa.fino_cur;
	error = xchk_btree(sc, cur, xchk_iallocbt_rec, &oinfo, &iabt);
	if (error)
		return error;

	xchk_iallocbt_xref_rmap_btreeblks(sc, which);

	/*
	 * If we're scrubbing the inode btree, inode_blocks is the number of
	 * blocks pointed to by all the inode chunk records.  Therefore, we
	 * should compare to the number of inode chunk blocks that the rmap
	 * knows about.  We can't do this for the finobt since it only points
	 * to inode chunks with free inodes.
	 */
	if (which == XFS_BTNUM_INO)
		xchk_iallocbt_xref_rmap_inodes(sc, which, iabt.inodes);

	return error;
}

int
xchk_inobt(
	struct xfs_scrub	*sc)
{
	return xchk_iallocbt(sc, XFS_BTNUM_INO);
}

int
xchk_finobt(
	struct xfs_scrub	*sc)
{
	return xchk_iallocbt(sc, XFS_BTNUM_FINO);
}

/* See if an inode btree has (or doesn't have) an inode chunk record. */
static inline void
xchk_xref_inode_check(
	struct xfs_scrub	*sc,
	xfs_agblock_t		agbno,
	xfs_extlen_t		len,
	struct xfs_btree_cur	**icur,
	bool			should_have_inodes)
{
	bool			has_inodes;
	int			error;

	if (!(*icur) || xchk_skip_xref(sc->sm))
		return;

	error = xfs_ialloc_has_inodes_at_extent(*icur, agbno, len, &has_inodes);
	if (!xchk_should_check_xref(sc, &error, icur))
		return;
	if (has_inodes != should_have_inodes)
		xchk_btree_xref_set_corrupt(sc, *icur, 0);
}

/* xref check that the extent is not covered by inodes */
void
xchk_xref_is_not_inode_chunk(
	struct xfs_scrub	*sc,
	xfs_agblock_t		agbno,
	xfs_extlen_t		len)
{
	xchk_xref_inode_check(sc, agbno, len, &sc->sa.ino_cur, false);
	xchk_xref_inode_check(sc, agbno, len, &sc->sa.fino_cur, false);
}

/* xref check that the extent is covered by inodes */
void
xchk_xref_is_inode_chunk(
	struct xfs_scrub	*sc,
	xfs_agblock_t		agbno,
	xfs_extlen_t		len)
{
	xchk_xref_inode_check(sc, agbno, len, &sc->sa.ino_cur, true);
}
