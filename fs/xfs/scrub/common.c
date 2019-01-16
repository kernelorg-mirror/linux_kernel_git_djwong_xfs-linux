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
#include "xfs_icache.h"
#include "xfs_itable.h"
#include "xfs_alloc.h"
#include "xfs_alloc_btree.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_ialloc.h"
#include "xfs_ialloc_btree.h"
#include "xfs_refcount.h"
#include "xfs_refcount_btree.h"
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_log.h"
#include "xfs_trans_priv.h"
#include "xfs_attr.h"
#include "xfs_reflink.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/btree.h"
#include "scrub/repair.h"

/* Common code for the metadata scrubbers. */

/*
 * Handling operational errors.
 *
 * The *_process_error() family of functions are used to process error return
 * codes from functions called as part of a scrub operation.
 *
 * If there's no error, we return true to tell the caller that it's ok
 * to move on to the next check in its list.
 *
 * For non-verifier errors (e.g. ENOMEM) we return false to tell the
 * caller that something bad happened, and we preserve *error so that
 * the caller can return the *error up the stack to userspace.
 *
 * Verifier errors (EFSBADCRC/EFSCORRUPTED) are recorded by setting
 * OFLAG_CORRUPT in sm_flags and the *error is cleared.  In other words,
 * we track verifier errors (and failed scrub checks) via OFLAG_CORRUPT,
 * not via return codes.  We return false to tell the caller that
 * something bad happened.  Since the error has been cleared, the caller
 * will (presumably) return that zero and scrubbing will move on to
 * whatever's next.
 *
 * ftrace can be used to record the precise metadata location and the
 * approximate code location of the failed operation.
 */

/* Check for operational errors. */
static bool
__xchk_process_error(
	struct xfs_scrub	*sc,
	xfs_agnumber_t		agno,
	xfs_agblock_t		bno,
	int			*error,
	__u32			errflag,
	void			*ret_ip)
{
	switch (*error) {
	case 0:
		return true;
	case -EDEADLOCK:
		/* Used to restart an op with deadlock avoidance. */
		trace_xchk_deadlock_retry(sc->ip, sc->sm, *error);
		break;
	case -EFSBADCRC:
	case -EFSCORRUPTED:
		/* Note the badness but don't abort. */
		sc->sm->sm_flags |= errflag;
		*error = 0;
		/* fall through */
	default:
		trace_xchk_op_error(sc, agno, bno, *error,
				ret_ip);
		break;
	}
	return false;
}

bool
xchk_process_error(
	struct xfs_scrub	*sc,
	xfs_agnumber_t		agno,
	xfs_agblock_t		bno,
	int			*error)
{
	return __xchk_process_error(sc, agno, bno, error,
			XFS_SCRUB_OFLAG_CORRUPT, __return_address);
}

bool
xchk_xref_process_error(
	struct xfs_scrub	*sc,
	xfs_agnumber_t		agno,
	xfs_agblock_t		bno,
	int			*error)
{
	return __xchk_process_error(sc, agno, bno, error,
			XFS_SCRUB_OFLAG_XFAIL, __return_address);
}

/* Check for operational errors for a file offset. */
static bool
__xchk_fblock_process_error(
	struct xfs_scrub	*sc,
	int			whichfork,
	xfs_fileoff_t		offset,
	int			*error,
	__u32			errflag,
	void			*ret_ip)
{
	switch (*error) {
	case 0:
		return true;
	case -EDEADLOCK:
		/* Used to restart an op with deadlock avoidance. */
		trace_xchk_deadlock_retry(sc->ip, sc->sm, *error);
		break;
	case -EFSBADCRC:
	case -EFSCORRUPTED:
		/* Note the badness but don't abort. */
		sc->sm->sm_flags |= errflag;
		*error = 0;
		/* fall through */
	default:
		trace_xchk_file_op_error(sc, whichfork, offset, *error,
				ret_ip);
		break;
	}
	return false;
}

bool
xchk_fblock_process_error(
	struct xfs_scrub	*sc,
	int			whichfork,
	xfs_fileoff_t		offset,
	int			*error)
{
	return __xchk_fblock_process_error(sc, whichfork, offset, error,
			XFS_SCRUB_OFLAG_CORRUPT, __return_address);
}

bool
xchk_fblock_xref_process_error(
	struct xfs_scrub	*sc,
	int			whichfork,
	xfs_fileoff_t		offset,
	int			*error)
{
	return __xchk_fblock_process_error(sc, whichfork, offset, error,
			XFS_SCRUB_OFLAG_XFAIL, __return_address);
}

/*
 * Handling scrub corruption/optimization/warning checks.
 *
 * The *_set_{corrupt,preen,warning}() family of functions are used to
 * record the presence of metadata that is incorrect (corrupt), could be
 * optimized somehow (preen), or should be flagged for administrative
 * review but is not incorrect (warn).
 *
 * ftrace can be used to record the precise metadata location and
 * approximate code location of the failed check.
 */

/* Record a block which could be optimized. */
void
xchk_block_set_preen(
	struct xfs_scrub	*sc,
	struct xfs_buf		*bp)
{
	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_PREEN;
	trace_xchk_block_preen(sc, bp->b_bn, __return_address);
}

/*
 * Record an inode which could be optimized.  The trace data will
 * include the block given by bp if bp is given; otherwise it will use
 * the block location of the inode record itself.
 */
void
xchk_ino_set_preen(
	struct xfs_scrub	*sc,
	xfs_ino_t		ino)
{
	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_PREEN;
	trace_xchk_ino_preen(sc, ino, __return_address);
}

/* Record a corrupt block. */
void
xchk_block_set_corrupt(
	struct xfs_scrub	*sc,
	struct xfs_buf		*bp)
{
	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
	trace_xchk_block_error(sc, bp->b_bn, __return_address);
}

/* Record a corruption while cross-referencing. */
void
xchk_block_xref_set_corrupt(
	struct xfs_scrub	*sc,
	struct xfs_buf		*bp)
{
	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_XCORRUPT;
	trace_xchk_block_error(sc, bp->b_bn, __return_address);
}

/*
 * Record a corrupt inode.  The trace data will include the block given
 * by bp if bp is given; otherwise it will use the block location of the
 * inode record itself.
 */
void
xchk_ino_set_corrupt(
	struct xfs_scrub	*sc,
	xfs_ino_t		ino)
{
	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
	trace_xchk_ino_error(sc, ino, __return_address);
}

/* Record a corruption while cross-referencing with an inode. */
void
xchk_ino_xref_set_corrupt(
	struct xfs_scrub	*sc,
	xfs_ino_t		ino)
{
	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_XCORRUPT;
	trace_xchk_ino_error(sc, ino, __return_address);
}

/* Record corruption in a block indexed by a file fork. */
void
xchk_fblock_set_corrupt(
	struct xfs_scrub	*sc,
	int			whichfork,
	xfs_fileoff_t		offset)
{
	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
	trace_xchk_fblock_error(sc, whichfork, offset, __return_address);
}

/* Record a corruption while cross-referencing a fork block. */
void
xchk_fblock_xref_set_corrupt(
	struct xfs_scrub	*sc,
	int			whichfork,
	xfs_fileoff_t		offset)
{
	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_XCORRUPT;
	trace_xchk_fblock_error(sc, whichfork, offset, __return_address);
}

/*
 * Warn about inodes that need administrative review but is not
 * incorrect.
 */
void
xchk_ino_set_warning(
	struct xfs_scrub	*sc,
	xfs_ino_t		ino)
{
	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_WARNING;
	trace_xchk_ino_warning(sc, ino, __return_address);
}

/* Warn about a block indexed by a file fork that needs review. */
void
xchk_fblock_set_warning(
	struct xfs_scrub	*sc,
	int			whichfork,
	xfs_fileoff_t		offset)
{
	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_WARNING;
	trace_xchk_fblock_warning(sc, whichfork, offset, __return_address);
}

/* Signal an incomplete scrub. */
void
xchk_set_incomplete(
	struct xfs_scrub	*sc)
{
	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_INCOMPLETE;
	trace_xchk_incomplete(sc, __return_address);
}

/*
 * rmap scrubbing -- compute the number of blocks with a given owner,
 * at least according to the reverse mapping data.
 */

struct xchk_rmap_ownedby_info {
	const struct xfs_owner_info	*oinfo;
	xfs_filblks_t			*blocks;
};

STATIC int
xchk_count_rmap_ownedby_irec(
	struct xfs_btree_cur		*cur,
	struct xfs_rmap_irec		*rec,
	void				*priv)
{
	struct xchk_rmap_ownedby_info	*sroi = priv;
	bool				irec_attr;
	bool				oinfo_attr;

	irec_attr = rec->rm_flags & XFS_RMAP_ATTR_FORK;
	oinfo_attr = sroi->oinfo->oi_flags & XFS_OWNER_INFO_ATTR_FORK;

	if (rec->rm_owner != sroi->oinfo->oi_owner)
		return 0;

	if (XFS_RMAP_NON_INODE_OWNER(rec->rm_owner) || irec_attr == oinfo_attr)
		(*sroi->blocks) += rec->rm_blockcount;

	return 0;
}

/*
 * Calculate the number of blocks the rmap thinks are owned by something.
 * The caller should pass us an rmapbt cursor.
 */
int
xchk_count_rmap_ownedby_ag(
	struct xfs_scrub		*sc,
	struct xfs_btree_cur		*cur,
	const struct xfs_owner_info	*oinfo,
	xfs_filblks_t			*blocks)
{
	struct xchk_rmap_ownedby_info	sroi = {
		.oinfo			= oinfo,
		.blocks			= blocks,
	};

	*blocks = 0;
	return xfs_rmap_query_all(cur, xchk_count_rmap_ownedby_irec,
			&sroi);
}

/*
 * AG scrubbing
 *
 * These helpers facilitate locking an allocation group's header
 * buffers, setting up cursors for all btrees that are present, and
 * cleaning everything up once we're through.
 */

/* Decide if we want to return an AG header read failure. */
static inline bool
want_ag_read_header_failure(
	struct xfs_scrub	*sc,
	unsigned int		type)
{
	/* Return all AG header read failures when scanning btrees. */
	if (sc->sm->sm_type != XFS_SCRUB_TYPE_AGF &&
	    sc->sm->sm_type != XFS_SCRUB_TYPE_AGFL &&
	    sc->sm->sm_type != XFS_SCRUB_TYPE_AGI)
		return true;
	/*
	 * If we're scanning a given type of AG header, we only want to
	 * see read failures from that specific header.  We'd like the
	 * other headers to cross-check them, but this isn't required.
	 */
	if (sc->sm->sm_type == type)
		return true;
	return false;
}

/*
 * Grab all the headers for an AG.
 *
 * The headers should be released by xchk_ag_free, but as a fail
 * safe we attach all the buffers we grab to the scrub transaction so
 * they'll all be freed when we cancel it.
 */
int
xchk_ag_read_headers(
	struct xfs_scrub	*sc,
	xfs_agnumber_t		agno,
	struct xfs_buf		**agi,
	struct xfs_buf		**agf,
	struct xfs_buf		**agfl)
{
	struct xfs_mount	*mp = sc->mp;
	int			error;

	error = xfs_ialloc_read_agi(mp, sc->tp, agno, agi);
	if (error && want_ag_read_header_failure(sc, XFS_SCRUB_TYPE_AGI))
		goto out;

	error = xfs_alloc_read_agf(mp, sc->tp, agno, 0, agf);
	if (error && want_ag_read_header_failure(sc, XFS_SCRUB_TYPE_AGF))
		goto out;

	error = xfs_alloc_read_agfl(mp, sc->tp, agno, agfl);
	if (error && want_ag_read_header_failure(sc, XFS_SCRUB_TYPE_AGFL))
		goto out;
	error = 0;
out:
	return error;
}

/* Release all the AG btree cursors. */
void
xchk_ag_btcur_free(
	struct xchk_ag		*sa)
{
	if (sa->refc_cur)
		xfs_btree_del_cursor(sa->refc_cur, XFS_BTREE_ERROR);
	if (sa->rmap_cur)
		xfs_btree_del_cursor(sa->rmap_cur, XFS_BTREE_ERROR);
	if (sa->fino_cur)
		xfs_btree_del_cursor(sa->fino_cur, XFS_BTREE_ERROR);
	if (sa->ino_cur)
		xfs_btree_del_cursor(sa->ino_cur, XFS_BTREE_ERROR);
	if (sa->cnt_cur)
		xfs_btree_del_cursor(sa->cnt_cur, XFS_BTREE_ERROR);
	if (sa->bno_cur)
		xfs_btree_del_cursor(sa->bno_cur, XFS_BTREE_ERROR);

	sa->refc_cur = NULL;
	sa->rmap_cur = NULL;
	sa->fino_cur = NULL;
	sa->ino_cur = NULL;
	sa->bno_cur = NULL;
	sa->cnt_cur = NULL;
}

/* Initialize all the btree cursors for an AG. */
int
xchk_ag_btcur_init(
	struct xfs_scrub	*sc,
	struct xchk_ag		*sa)
{
	struct xfs_mount	*mp = sc->mp;
	xfs_agnumber_t		agno = sa->agno;

	if (sa->agf_bp) {
		/* Set up a bnobt cursor for cross-referencing. */
		sa->bno_cur = xfs_allocbt_init_cursor(mp, sc->tp, sa->agf_bp,
				agno, XFS_BTNUM_BNO);
		if (!sa->bno_cur)
			goto err;

		/* Set up a cntbt cursor for cross-referencing. */
		sa->cnt_cur = xfs_allocbt_init_cursor(mp, sc->tp, sa->agf_bp,
				agno, XFS_BTNUM_CNT);
		if (!sa->cnt_cur)
			goto err;
	}

	/* Set up a inobt cursor for cross-referencing. */
	if (sa->agi_bp) {
		sa->ino_cur = xfs_inobt_init_cursor(mp, sc->tp, sa->agi_bp,
					agno, XFS_BTNUM_INO);
		if (!sa->ino_cur)
			goto err;
	}

	/* Set up a finobt cursor for cross-referencing. */
	if (sa->agi_bp && xfs_sb_version_hasfinobt(&mp->m_sb)) {
		sa->fino_cur = xfs_inobt_init_cursor(mp, sc->tp, sa->agi_bp,
				agno, XFS_BTNUM_FINO);
		if (!sa->fino_cur)
			goto err;
	}

	/* Set up a rmapbt cursor for cross-referencing. */
	if (sa->agf_bp && xfs_sb_version_hasrmapbt(&mp->m_sb)) {
		sa->rmap_cur = xfs_rmapbt_init_cursor(mp, sc->tp, sa->agf_bp,
				agno);
		if (!sa->rmap_cur)
			goto err;
	}

	/* Set up a refcountbt cursor for cross-referencing. */
	if (sa->agf_bp && xfs_sb_version_hasreflink(&mp->m_sb)) {
		sa->refc_cur = xfs_refcountbt_init_cursor(mp, sc->tp,
				sa->agf_bp, agno);
		if (!sa->refc_cur)
			goto err;
	}

	return 0;
err:
	return -ENOMEM;
}

/* Release the AG header context and btree cursors. */
void
xchk_ag_free(
	struct xfs_scrub	*sc,
	struct xchk_ag		*sa)
{
	xchk_ag_btcur_free(sa);
	if (sa->pag != NULL && sc->reset_perag_resv)
		xrep_reset_perag_resv(sc);
	if (sa->agfl_bp) {
		xfs_trans_brelse(sc->tp, sa->agfl_bp);
		sa->agfl_bp = NULL;
	}
	if (sa->agf_bp) {
		xfs_trans_brelse(sc->tp, sa->agf_bp);
		sa->agf_bp = NULL;
	}
	if (sa->agi_bp) {
		xfs_trans_brelse(sc->tp, sa->agi_bp);
		sa->agi_bp = NULL;
	}
	if (sa->pag) {
		xfs_perag_put(sa->pag);
		sa->pag = NULL;
	}
	sa->agno = NULLAGNUMBER;
}

/*
 * For scrub, grab the AGI and the AGF headers, in that order.  Locking
 * order requires us to get the AGI before the AGF.  We use the
 * transaction to avoid deadlocking on crosslinked metadata buffers;
 * either the caller passes one in (bmap scrub) or we have to create a
 * transaction ourselves.
 */
int
xchk_ag_init(
	struct xfs_scrub	*sc,
	xfs_agnumber_t		agno,
	struct xchk_ag		*sa)
{
	int			error;

	sa->agno = agno;
	error = xchk_ag_read_headers(sc, agno, &sa->agi_bp,
			&sa->agf_bp, &sa->agfl_bp);
	if (error)
		return error;

	return xchk_ag_btcur_init(sc, sa);
}

/*
 * Grab the per-ag structure if we haven't already gotten it.  Teardown of the
 * xchk_ag will release it for us.
 */
void
xchk_perag_get(
	struct xfs_mount	*mp,
	struct xchk_ag		*sa)
{
	if (!sa->pag)
		sa->pag = xfs_perag_get(mp, sa->agno);
}

/* Per-scrubber setup functions */

/*
 * Grab an empty transaction so that we can re-grab locked buffers if
 * one of our btrees turns out to be cyclic.
 *
 * If we're going to repair something, we need to ask for the largest possible
 * log reservation so that we can handle the worst case scenario for metadata
 * updates while rebuilding a metadata item.  We also need to reserve as many
 * blocks in the head transaction as we think we're going to need to rebuild
 * the metadata object.
 */
int
xchk_trans_alloc(
	struct xfs_scrub	*sc,
	uint			resblks)
{
	uint			flags = 0;

	if (sc->fs_frozen)
		flags |= XFS_TRANS_NO_WRITECOUNT;
	if (sc->sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR)
		return xfs_trans_alloc(sc->mp, &M_RES(sc->mp)->tr_itruncate,
				resblks, 0, flags, &sc->tp);

	return xfs_trans_alloc_empty(sc->mp, &sc->tp);
}

/* Set us up with a transaction and an empty context. */
int
xchk_setup_fs(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	uint			resblks;

	resblks = xrep_calc_ag_resblks(sc);
	return xchk_trans_alloc(sc, resblks);
}

/* Set us up with AG headers and btree cursors. */
int
xchk_setup_ag_btree(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip,
	bool			force_log)
{
	struct xfs_mount	*mp = sc->mp;
	int			error;

	/*
	 * If the caller asks us to checkpont the log, do so.  This
	 * expensive operation should be performed infrequently and only
	 * as a last resort.  Any caller that sets force_log should
	 * document why they need to do so.
	 *
	 * Force everything in memory out to disk if we're repairing.
	 * This ensures we won't get tripped up by btree blocks sitting
	 * in memory waiting to have LSNs stamped in.  The AGF/AGI repair
	 * routines use any available rmap data to try to find a btree
	 * root that also passes the read verifiers.
	 */
	if (force_log || (sc->sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR)) {
		error = xchk_checkpoint_log(mp);
		if (error)
			return error;
	}

	error = xchk_setup_fs(sc, ip);
	if (error)
		return error;

	return xchk_ag_init(sc, sc->sm->sm_agno, &sc->sa);
}

/* Push everything out of the log onto disk. */
int
xchk_checkpoint_log(
	struct xfs_mount	*mp)
{
	int			error;

	error = xfs_log_force(mp, XFS_LOG_SYNC);
	if (error)
		return error;
	xfs_ail_push_all_sync(mp->m_ail);
	return 0;
}

/*
 * Given an inode and the scrub control structure, grab either the
 * inode referenced in the control structure or the inode passed in.
 * The inode is not locked.
 */
int
xchk_get_inode(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip_in)
{
	struct xfs_imap		imap;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_inode	*ip = NULL;
	int			error;

	/* We want to scan the inode we already had opened. */
	if (sc->sm->sm_ino == 0 || sc->sm->sm_ino == ip_in->i_ino) {
		sc->ip = ip_in;
		return 0;
	}

	/* Look up the inode, see if the generation number matches. */
	if (xfs_internal_inum(mp, sc->sm->sm_ino))
		return -ENOENT;
	error = xfs_iget(mp, NULL, sc->sm->sm_ino,
			XFS_IGET_UNTRUSTED | XFS_IGET_DONTCACHE, 0, &ip);
	switch (error) {
	case -ENOENT:
		/* Inode doesn't exist, just bail out. */
		return error;
	case 0:
		/* Got an inode, continue. */
		break;
	case -EINVAL:
		/*
		 * -EINVAL with IGET_UNTRUSTED could mean one of several
		 * things: userspace gave us an inode number that doesn't
		 * correspond to fs space, or doesn't have an inobt entry;
		 * or it could simply mean that the inode buffer failed the
		 * read verifiers.
		 *
		 * Try just the inode mapping lookup -- if it succeeds, then
		 * the inode buffer verifier failed and something needs fixing.
		 * Otherwise, we really couldn't find it so tell userspace
		 * that it no longer exists.
		 */
		error = xfs_imap(sc->mp, sc->tp, sc->sm->sm_ino, &imap,
				XFS_IGET_UNTRUSTED | XFS_IGET_DONTCACHE);
		if (error)
			return -ENOENT;
		error = -EFSCORRUPTED;
		/* fall through */
	default:
		trace_xchk_op_error(sc,
				XFS_INO_TO_AGNO(mp, sc->sm->sm_ino),
				XFS_INO_TO_AGBNO(mp, sc->sm->sm_ino),
				error, __return_address);
		return error;
	}
	if (VFS_I(ip)->i_generation != sc->sm->sm_gen) {
		xfs_irele(ip);
		return -ENOENT;
	}

	sc->ip = ip;
	return 0;
}

/* Set us up to scrub a file's contents. */
int
xchk_setup_inode_contents(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip,
	unsigned int		resblks)
{
	int			error;

	error = xchk_get_inode(sc, ip);
	if (error)
		return error;

	/* Got the inode, lock it and we're ready to go. */
	sc->ilock_flags = XFS_IOLOCK_EXCL | XFS_MMAPLOCK_EXCL;
	xfs_ilock(sc->ip, sc->ilock_flags);
	error = xchk_trans_alloc(sc, resblks);
	if (error)
		goto out;
	sc->ilock_flags |= XFS_ILOCK_EXCL;
	xfs_ilock(sc->ip, XFS_ILOCK_EXCL);

out:
	/* scrub teardown will unlock and release the inode for us */
	return error;
}

/*
 * Predicate that decides if we need to evaluate the cross-reference check.
 * If there was an error accessing the cross-reference btree, just delete
 * the cursor and skip the check.
 */
bool
xchk_should_check_xref(
	struct xfs_scrub	*sc,
	int			*error,
	struct xfs_btree_cur	**curpp)
{
	/* No point in xref if we already know we're corrupt. */
	if (xchk_skip_xref(sc->sm))
		return false;

	if (*error == 0)
		return true;

	if (curpp) {
		/* If we've already given up on xref, just bail out. */
		if (!*curpp)
			return false;

		/* xref error, delete cursor and bail out. */
		xfs_btree_del_cursor(*curpp, XFS_BTREE_ERROR);
		*curpp = NULL;
	}

	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_XFAIL;
	trace_xchk_xref_error(sc, *error, __return_address);

	/*
	 * Errors encountered during cross-referencing with another
	 * data structure should not cause this scrubber to abort.
	 */
	*error = 0;
	return false;
}

/* Run the structure verifiers on in-memory buffers to detect bad memory. */
void
xchk_buffer_recheck(
	struct xfs_scrub	*sc,
	struct xfs_buf		*bp)
{
	xfs_failaddr_t		fa;

	if (bp->b_ops == NULL) {
		xchk_block_set_corrupt(sc, bp);
		return;
	}
	if (bp->b_ops->verify_struct == NULL) {
		xchk_set_incomplete(sc);
		return;
	}
	fa = bp->b_ops->verify_struct(bp);
	if (!fa)
		return;
	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
	trace_xchk_block_error(sc, bp->b_bn, fa);
}

/*
 * Scrub the attr/data forks of a metadata inode.  The metadata inode must be
 * pointed to by sc->ip and the ILOCK must be held.
 */
int
xchk_metadata_inode_forks(
	struct xfs_scrub	*sc)
{
	__u32			smtype;
	bool			shared;
	int			error;

	if (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		return 0;

	/* Metadata inodes don't live on the rt device. */
	if (sc->ip->i_d.di_flags & XFS_DIFLAG_REALTIME) {
		xchk_ino_set_corrupt(sc, sc->ip->i_ino);
		return 0;
	}

	/* They should never participate in reflink. */
	if (xfs_is_reflink_inode(sc->ip)) {
		xchk_ino_set_corrupt(sc, sc->ip->i_ino);
		return 0;
	}

	/* They also should never have extended attributes. */
	if (xfs_inode_hasattr(sc->ip)) {
		xchk_ino_set_corrupt(sc, sc->ip->i_ino);
		return 0;
	}

	/* Invoke the data fork scrubber. */
	smtype = sc->sm->sm_type;
	sc->sm->sm_type = XFS_SCRUB_TYPE_BMBTD;
	error = xchk_bmap_data(sc);
	sc->sm->sm_type = smtype;
	if (error || (sc->sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT))
		return error;

	/* Look for incorrect shared blocks. */
	if (xfs_sb_version_hasreflink(&sc->mp->m_sb)) {
		error = xfs_reflink_inode_has_shared_extents(sc->tp, sc->ip,
				&shared);
		if (!xchk_fblock_process_error(sc, XFS_DATA_FORK, 0,
				&error))
			return error;
		if (shared)
			xchk_ino_set_corrupt(sc, sc->ip->i_ino);
	}

	return error;
}

/*
 * Try to lock an inode in violation of the usual locking order rules.  For
 * example, trying to get the IOLOCK while in transaction context, or just
 * plain breaking AG-order or inode-order inode locking rules.  Either way,
 * the only way to avoid an ABBA deadlock is to use trylock and back off if
 * we can't.
 */
int
xchk_ilock_inverted(
	struct xfs_inode	*ip,
	uint			lock_mode)
{
	int			i;

	for (i = 0; i < 20; i++) {
		if (xfs_ilock_nowait(ip, lock_mode))
			return 0;
		delay(1);
	}
	return -EDEADLOCK;
}

/*
 * Exclusive Filesystem Access During Scrub and Repair
 * ===================================================
 *
 * While most scrub activity can occur while the filesystem is live, there
 * are certain scenarios where we cannot tolerate concurrent metadata updates.
 * We therefore must freeze the filesystem against all other changes.
 *
 * The typical scenarios envisioned for scrub freezes are (a) to lock out all
 * other filesystem changes in order to check the global summary counters,
 * and anything else that requires unusual behavioral semantics.
 *
 * The typical scenarios envisioned for repair freezes are (a) to avoid ABBA
 * deadlocks when need to take locks in an unusual order; or (b) to update
 * global filesystem state.  For example, reconstruction of a damaged reverse
 * mapping btree requires us to hold the AG header locks while scanning
 * inodes, which goes against the usual inode -> AG header locking order.
 *
 * A note about inode reclaim: when we freeze the filesystem, users can't
 * modify things and periodic background reclaim of speculative preallocations
 * and copy-on-write staging extents is stopped.  However, the scrub/repair
 * thread must be careful about evicting an inode from memory -- if the
 * eviction would require a transaction, we must defer the iput until after
 * the scrub freeze.  The reasons for this are twofold: first, scrub/repair
 * already have a transaction and xfs can't nest transactions; and second, we
 * froze the fs to prevent modifications that we can't control directly.
 * This guarantee is made by freezing the inode inactivation worker while
 * frozen.
 *
 * Userspace is prevented from freezing or thawing the filesystem during a
 * repair freeze by the ->freeze_super and ->thaw_super superblock operations,
 * which block any changes to the freeze state while a repair freeze is
 * running through the use of the m_scrub_freeze mutex.  It only makes sense
 * to run one scrub/repair freeze at a time, so the mutex is fine.
 *
 * Scrub/repair freezes cannot be initiated during a regular freeze because
 * freeze_super does not allow nested freeze.  Repair activity that does not
 * require a repair freeze is also prevented from running during a regular
 * freeze because transaction allocation blocks on the regular freeze.  We
 * assume that the only other users of XFS_TRANS_NO_WRITECOUNT transactions
 * either aren't modifying space metadata in a way that would affect repair,
 * or that we can inhibit any of the ones that do.
 *
 * Note that thaw_super and freeze_super can call deactivate_locked_super
 * which can free the xfs_mount.  This can happen if someone freezes the block
 * device, unmounts the filesystem, and thaws the block device.  Therefore, we
 * must be careful about who gets to unlock the repair freeze mutex.  See the
 * comments in xfs_fs_put_super.
 */

/* Start a scrub/repair freeze. */
int
xfs_scrub_fs_freeze(
	struct xfs_scrub	*sc)
{
	int			error;

	if (!(sc->sm->sm_flags & XFS_SCRUB_IFLAG_FREEZE_OK))
		return -EUSERS;

	mutex_lock(&sc->mp->m_scrub_freeze);
	error = freeze_super(sc->mp->m_super);
	if (error) {
		mutex_unlock(&sc->mp->m_scrub_freeze);
		return error;
	}
	sc->fs_frozen = true;
	return 0;
}

/* Release a scrub/repair freeze. */
int
xfs_scrub_fs_thaw(
	struct xfs_scrub	*sc)
{
	int			error;

	sc->fs_frozen = false;
	error = thaw_super(sc->mp->m_super);
	mutex_unlock(&sc->mp->m_scrub_freeze);
	return error;
}

/* Decide if we're going to grab this inode for iteration. */
STATIC int
xfs_scrub_foreach_live_inode_ag_grab(
	struct xfs_inode	*ip)
{
	struct inode		*inode = VFS_I(ip);

	ASSERT(rcu_read_lock_held());

	/*
	 * Check for stale RCU freed inode
	 *
	 * If the inode has been reallocated, it doesn't matter if it's not in
	 * the AG we are walking - we are walking for writeback, so if it
	 * passes all the "valid inode" checks and is dirty, then we'll write
	 * it back anyway.  If it has been reallocated and still being
	 * initialised, the XFS_INEW check below will catch it.
	 */
	spin_lock(&ip->i_flags_lock);
	if (!ip->i_ino)
		goto out_unlock_noent;

	/* Avoid new or reclaimable inodes. Leave for reclaim code to flush */
	if (__xfs_iflags_test(ip, XFS_INEW | XFS_IRECLAIMABLE | XFS_IRECLAIM))
		goto out_unlock_noent;
	spin_unlock(&ip->i_flags_lock);

	/* Nothing to sync during shutdown */
	if (XFS_FORCED_SHUTDOWN(ip->i_mount))
		return -EFSCORRUPTED;

	/* If we can't grab the inode, it must on it's way to reclaim. */
	if (!igrab(inode))
		return -ENOENT;

	/* inode is valid */
	return 0;

out_unlock_noent:
	spin_unlock(&ip->i_flags_lock);
	return -ENOENT;
}

#define XFS_LOOKUP_BATCH 32
/*
 * Iterate all in-core inodes of an AG.  We will not wait for inodes that are
 * new or reclaimable, and the filesystem should be frozen by the caller.
 */
STATIC int
xfs_scrub_foreach_live_inode_ag(
	struct xfs_scrub	*sc,
	struct xfs_perag	*pag,
	int			(*execute)(struct xfs_inode *ip, void *priv),
	void			*priv)
{
	struct xfs_mount	*mp = sc->mp;
	uint32_t		first_index = 0;
	int			done = 0;
	int			nr_found = 0;
	int			error = 0;

	do {
		struct xfs_inode *batch[XFS_LOOKUP_BATCH];
		int		i;

		rcu_read_lock();

		nr_found = radix_tree_gang_lookup(&pag->pag_ici_root,
				(void **)batch, first_index, XFS_LOOKUP_BATCH);
		if (!nr_found) {
			rcu_read_unlock();
			break;
		}

		/*
		 * Grab the inodes before we drop the lock. if we found
		 * nothing, nr == 0 and the loop will be skipped.
		 */
		for (i = 0; i < nr_found; i++) {
			struct xfs_inode *ip = batch[i];

			if (done || xfs_scrub_foreach_live_inode_ag_grab(ip))
				batch[i] = NULL;

			/*
			 * Update the index for the next lookup. Catch
			 * overflows into the next AG range which can occur if
			 * we have inodes in the last block of the AG and we
			 * are currently pointing to the last inode.
			 *
			 * Because we may see inodes that are from the wrong AG
			 * due to RCU freeing and reallocation, only update the
			 * index if it lies in this AG. It was a race that lead
			 * us to see this inode, so another lookup from the
			 * same index will not find it again.
			 */
			if (XFS_INO_TO_AGNO(mp, ip->i_ino) != pag->pag_agno)
				continue;
			first_index = XFS_INO_TO_AGINO(mp, ip->i_ino + 1);
			if (first_index < XFS_INO_TO_AGINO(mp, ip->i_ino))
				done = 1;
		}

		/* unlock now we've grabbed the inodes. */
		rcu_read_unlock();

		for (i = 0; i < nr_found; i++) {
			if (!batch[i])
				continue;
			if (!error)
				error = execute(batch[i], priv);
			xfs_irele(batch[i]);
		}

		if (error)
			break;
	} while (nr_found && !done);

	return error;
}

/*
 * Iterate all in-core inodes.  We will not wait for inodes that are
 * new or reclaimable, and the filesystem should be frozen by the caller.
 */
int
xfs_scrub_foreach_live_inode(
	struct xfs_scrub	*sc,
	int			(*execute)(struct xfs_inode *ip, void *priv),
	void			*priv)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno;
	int			error = 0;

	for (agno = 0; agno < mp->m_sb.sb_agcount && !error; agno++) {
		pag = xfs_perag_get(mp, agno);
		error = xfs_scrub_foreach_live_inode_ag(sc, pag, execute, priv);
		xfs_perag_put(pag);
	}

	return error;
}
