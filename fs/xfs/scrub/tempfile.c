// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_inode.h"
#include "xfs_ialloc.h"
#include "xfs_quota.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans_space.h"
#include "xfs_dir2.h"
#include "xfs_xchgrange.h"
#include "xfs_swapext.h"
#include "xfs_defer.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/repair.h"
#include "scrub/trace.h"
#include "scrub/tempfile.h"
#include "scrub/xfile.h"

/*
 * Create a temporary file for reconstructing metadata, with the intention of
 * atomically swapping the temporary file's contents with the file that's
 * being repaired.
 */
int
xrep_tempfile_create(
	struct xfs_scrub	*sc,
	uint16_t		mode)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_trans	*tp = NULL;
	struct xfs_dquot	*udqp = NULL;
	struct xfs_dquot	*gdqp = NULL;
	struct xfs_dquot	*pdqp = NULL;
	struct xfs_trans_res	*tres;
	struct xfs_inode	*dp = mp->m_rootip;
	xfs_ino_t		ino;
	unsigned int		resblks;
	bool			is_dir = S_ISDIR(mode);
	bool			use_log = false;
	int			error;

	if (xfs_is_shutdown(mp))
		return -EIO;
	if (xfs_is_readonly(mp))
		return -EROFS;

	ASSERT(sc->tp == NULL);
	ASSERT(sc->tempip == NULL);

	/* Enable atomic extent swapping. */
	error = xfs_xchg_range_grab_log_assist(mp, true, &use_log);
	if (error)
		return error;
	ASSERT(use_log);
	sc->flags |= XREP_ATOMIC_EXCHANGE;

	/*
	 * Make sure that we have allocated dquot(s) on disk.  The temporary
	 * inode should be completely root owned so that we don't fail due to
	 * quota limits.
	 */
	error = xfs_qm_vop_dqalloc(dp, GLOBAL_ROOT_UID, GLOBAL_ROOT_GID, 0,
			XFS_QMOPT_QUOTALL, &udqp, &gdqp, &pdqp);
	if (error)
		return error;

	if (is_dir) {
		resblks = XFS_MKDIR_SPACE_RES(mp, 0);
		tres = &M_RES(mp)->tr_mkdir;
	} else {
		resblks = XFS_IALLOC_SPACE_RES(mp);
		tres = &M_RES(mp)->tr_create_tmpfile;
	}

	error = xfs_trans_alloc_icreate(mp, tres, udqp, gdqp, pdqp, resblks,
			&tp);
	if (error)
		goto out_release_dquots;

	/* Allocate inode, set up directory. */
	error = xfs_dialloc(&tp, dp->i_ino, mode, &ino);
	if (error)
		goto out_trans_cancel;
	error = xfs_init_new_inode(&init_user_ns, tp, dp, ino, mode, 0, 0,
			0, false, &sc->tempip);
	if (error)
		goto out_trans_cancel;

	/* Change the ownership of the inode to root. */
	VFS_I(sc->tempip)->i_uid = GLOBAL_ROOT_UID;
	VFS_I(sc->tempip)->i_gid = GLOBAL_ROOT_GID;
	sc->tempip->i_diflags &= ~(XFS_DIFLAG_REALTIME | XFS_DIFLAG_RTINHERIT);
	xfs_trans_log_inode(tp, sc->tempip, XFS_ILOG_CORE);

	/*
	 * Mark our temporary file as private so that LSMs and the ACL code
	 * don't try to add their own metadata or reason about these files.
	 * The file should never be exposed to userspace.
	 */
	VFS_I(sc->tempip)->i_flags |= S_PRIVATE;
	VFS_I(sc->tempip)->i_opflags &= ~IOP_XATTR;

	if (is_dir) {
		error = xfs_dir_init(tp, sc->tempip, dp);
		if (error)
			goto out_trans_cancel;
	}

	/*
	 * Attach the dquot(s) to the inodes and modify them incore.
	 * These ids of the inode couldn't have changed since the new
	 * inode has been locked ever since it was created.
	 */
	xfs_qm_vop_create_dqattach(tp, sc->tempip, udqp, gdqp, pdqp);

	/*
	 * Put our temp file on the unlinked list so it's purged automatically.
	 * Anything being reconstructed using this file must be atomically
	 * swapped with the original file because the contents here will be
	 * purged when the inode is dropped or log recovery cleans out the
	 * unlinked list.
	 */
	error = xfs_iunlink(tp, sc->tempip);
	if (error)
		goto out_trans_cancel;

	error = xfs_trans_commit(tp);
	if (error)
		goto out_release_inode;

	trace_xrep_tempfile_create(sc);

	xfs_qm_dqrele(udqp);
	xfs_qm_dqrele(gdqp);
	xfs_qm_dqrele(pdqp);

	/* Finish setting up the incore / vfs context. */
	xfs_setup_iops(sc->tempip);
	xfs_finish_inode_setup(sc->tempip);

	sc->temp_ilock_flags = 0;
	return error;

out_trans_cancel:
	xfs_trans_cancel(tp);
out_release_inode:
	/*
	 * Wait until after the current transaction is aborted to finish the
	 * setup of the inode and release the inode.  This prevents recursive
	 * transactions and deadlocks from xfs_inactive.
	 */
	if (sc->tempip) {
		xfs_finish_inode_setup(sc->tempip);
		xchk_irele(sc, sc->tempip);
	}
out_release_dquots:
	xfs_qm_dqrele(udqp);
	xfs_qm_dqrele(gdqp);
	xfs_qm_dqrele(pdqp);

	return error;
}

void
xrep_tempfile_ilock(
	struct xfs_scrub	*sc,
	unsigned int		ilock_flags)
{
	sc->temp_ilock_flags |= ilock_flags;
	xfs_ilock(sc->tempip, ilock_flags);
}

bool
xrep_tempfile_ilock_nowait(
	struct xfs_scrub	*sc,
	unsigned int		ilock_flags)
{
	if (xfs_ilock_nowait(sc->tempip, ilock_flags)) {
		sc->temp_ilock_flags |= ilock_flags;
		return true;
	}

	return false;
}

void
xrep_tempfile_iunlock(
	struct xfs_scrub	*sc,
	unsigned int		ilock_flags)
{
	xfs_iunlock(sc->tempip, ilock_flags);
	sc->temp_ilock_flags &= ~ilock_flags;
}

/* Release the temporary file. */
void
xrep_tempfile_rele(
	struct xfs_scrub	*sc)
{
	if (!sc->tempip)
		return;

	if (sc->temp_ilock_flags)
		xrep_tempfile_iunlock(sc, sc->temp_ilock_flags);

	xchk_irele(sc, sc->tempip);
	sc->tempip = NULL;
}

/*
 * Make sure that the given range of the data fork of the temporary file is
 * mapped to written blocks.  The caller must ensure that both inodes are
 * joined to the transaction.
 */
int
xrep_tempfile_prealloc(
	struct xfs_scrub	*sc,
	xfs_fileoff_t		off,
	xfs_filblks_t		len)
{
	xfs_fileoff_t		end = off + len;
	int			error = 0;

	ASSERT(sc->tempip != NULL);
	ASSERT(!XFS_NOT_DQATTACHED(sc->mp, sc->tempip));

	while (off < len) {
		struct xfs_bmbt_irec	map;
		int			nmaps = 1;

		/*
		 * If we have a real extent mapping this block then we're
		 * in ok shape.
		 */
		error = xfs_bmapi_read(sc->tempip, off, end - off, &map, &nmaps,
				XFS_DATA_FORK);
		if (error)
			break;

		if (nmaps == 1 && xfs_bmap_is_written_extent(&map)) {
			off += map.br_startblock;
			continue;
		}

		/*
		 * If we find a delalloc reservation then something is very
		 * very wrong.  Bail out.
		 */
		if (map.br_startblock == DELAYSTARTBLOCK)
			return -EFSCORRUPTED;

		/*
		 * Make sure this block has a real zeroed extent allocated to
		 * it.
		 */
		nmaps = 1;
		error = xfs_bmapi_write(sc->tp, sc->tempip, off, end - off,
				XFS_BMAPI_CONVERT | XFS_BMAPI_ZERO, 0, &map,
				&nmaps);
		if (error)
			break;

		trace_xrep_tempfile_prealloc(sc, XFS_DATA_FORK, &map);

		/* Commit new extent and all deferred work. */
		error = xfs_defer_finish(&sc->tp);
		if (error)
			break;

		off += map.br_startblock;
	}

	return error;
}

/*
 * Write a number of bytes from the xfile into the temp file, one filesystem
 * block at a time.  The caller must join both inodes to the transaction.
 */
int
xrep_tempfile_copyin_xfile(
	struct xfs_scrub		*sc,
	const struct xfs_buf_ops	*ops,
	enum xfs_blft			type,
	xfs_fileoff_t			isize)
{
	LIST_HEAD(buffers_list);
	struct xfs_mount		*mp = sc->mp;
	struct xfs_buf			*bp;
	xfs_fileoff_t			flush_mask;
	xfs_rtblock_t			off = 0;
	loff_t				pos = 0;
	int				error = 0;

	ASSERT(S_ISREG(VFS_I(sc->tempip)->i_mode));

	/* Flush buffers to disk every 512K */
	flush_mask = XFS_B_TO_FSBT(mp, (1U << 19)) - 1;

	while (pos < isize) {
		struct xfs_bmbt_irec	map;
		int			nmaps = 1;
		size_t			count;

		/* Read block mapping for this file block. */
		error = xfs_bmapi_read(sc->tempip, off, 1, &map, &nmaps, 0);
		if (error)
			goto out_err;
		if (nmaps == 0 || !xfs_bmap_is_written_extent(&map)) {
			error = -EFSCORRUPTED;
			goto out_err;
		}

		/* Get the metadata buffer for this offset in the file. */
		error = xfs_trans_get_buf(sc->tp, mp->m_ddev_targp,
				XFS_FSB_TO_DADDR(mp, map.br_startblock),
				mp->m_bsize, 0, &bp);
		if (error)
			goto out_err;
		bp->b_ops = ops;
		xfs_trans_buf_set_type(sc->tp, bp, type);

		/* Read in a block's worth of data from the xfile. */
		count = min_t(loff_t, isize - pos, mp->m_sb.sb_blocksize);
		error = xfile_obj_load(sc->xfile, bp->b_addr, count, pos);
		if (error) {
			xfs_trans_brelse(sc->tp, bp);
			goto out_err;
		}

		trace_xrep_tempfile_copyin_xfile(sc, XFS_DATA_FORK, &map);

		/* Queue buffer, and flush if we have too much dirty data. */
		xfs_buf_delwri_queue_here(bp, &buffers_list);
		xfs_trans_brelse(sc->tp, bp);

		if (!(off & flush_mask)) {
			error = xfs_buf_delwri_submit(&buffers_list);
			if (error)
				goto out_err;
		}

		pos += mp->m_sb.sb_blocksize;
		off++;
	}

	/*
	 * Write the new blocks to disk.  If the ordered list isn't empty after
	 * that, then something went wrong and we have to fail.  This should
	 * never happen, but we'll check anyway.
	 */
	error = xfs_buf_delwri_submit(&buffers_list);
	if (error)
		goto out_err;

	if (!list_empty(&buffers_list)) {
		ASSERT(list_empty(&buffers_list));
		error = -EIO;
		goto out_err;
	}

	/* Set the new inode size, if needed. */
	if (sc->tempip->i_disk_size != isize) {
		sc->tempip->i_disk_size = isize;
		i_size_write(VFS_I(sc->tempip), isize);
		xfs_trans_log_inode(sc->tp, sc->tempip, XFS_ILOG_CORE);
		return xrep_roll_trans(sc);
	}

	return 0;

out_err:
	xfs_buf_delwri_cancel(&buffers_list);
	return error;
}

/*
 * Fill out the swapext request in preparation for swapping the contents of a
 * metadata file that we've rebuilt in the temp file.
 */
int
xrep_tempfile_swapext_prep_request(
	struct xfs_scrub	*sc,
	int			whichfork,
	struct xfs_swapext_req	*req)
{
	/* COW forks don't exist on disk. */
	if (whichfork == XFS_COW_FORK) {
		ASSERT(0);
		return -EINVAL;
	}

	/* Both files should have the relevant forks. */
	if (!XFS_IFORK_PTR(sc->ip, whichfork) ||
	    !XFS_IFORK_PTR(sc->tempip, whichfork)) {
		ASSERT(0);
		return -EINVAL;
	}

	/* Swap all mappings in both forks. */
	req->ip1 = sc->tempip;
	req->ip2 = sc->ip;
	req->startoff1 = 0;
	req->startoff2 = 0;
	req->whichfork = whichfork;
	req->blockcount = XFS_MAX_FILEOFF;
	req->req_flags = 0;

	/* Always swap sizes when we're swapping data fork mappings. */
	if (whichfork == XFS_DATA_FORK)
		req->req_flags |= XFS_SWAP_REQ_SET_SIZES;

	/*
	 * If we're repairing xattrs or directories, always try to convert ip2
	 * to short format after swapping.
	 */
	if (whichfork == XFS_ATTR_FORK || S_ISDIR(VFS_I(sc->ip)->i_mode))
		req->req_flags |= XFS_SWAP_REQ_FILE2_CVT_SF;

	return 0;
}

/* Swap forks between the file being repaired and the temporary file. */
int
xrep_tempfile_swapext(
	struct xfs_scrub	*sc,
	struct xfs_swapext_req	*req)
{
	int			error;

	error = xfs_swapext(&sc->tp, req);
	if (error)
		return error;

	/*
	 * If we swapped the ondisk sizes of two metadata files, we must swap
	 * the incore sizes as well.  Since online fsck doesn't use swapext on
	 * the data forks of user-accessible files, the two sizes are always
	 * the same, so we don't need to log the inodes.
	 */
	if (req->req_flags & XFS_SWAP_REQ_SET_SIZES) {
		loff_t	temp;

		temp = i_size_read(VFS_I(sc->ip));
		i_size_write(VFS_I(sc->ip), i_size_read(VFS_I(sc->tempip)));
		i_size_write(VFS_I(sc->tempip), temp);
	}

	return 0;
}
