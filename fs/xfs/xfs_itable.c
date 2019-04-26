// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2002,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_btree.h"
#include "xfs_ialloc.h"
#include "xfs_ialloc_btree.h"
#include "xfs_itable.h"
#include "xfs_error.h"
#include "xfs_trace.h"
#include "xfs_icache.h"
#include "xfs_health.h"
#include "xfs_iwalk.h"

/*
 * Bulk Stat
 * =========
 *
 * Use the inode walking functions to fill out struct xfs_bstat for every
 * allocated inode, then pass the stat information to some externally provided
 * iteration function.
 */

struct xfs_bstat_chunk {
	bulkstat_one_fmt_pf	formatter;
	struct xfs_ibulk	*breq;
};

/*
 * Fill out the bulkstat info for a single inode and report it somewhere.
 *
 * bc->breq->lastino is effectively the inode cursor as we walk through the
 * filesystem.  Therefore, we update it any time we need to move the cursor
 * forward, regardless of whether or not we're sending any bstat information
 * back to userspace.  If the inode is internal metadata or, has been freed
 * out from under us, we just simply keep going.
 *
 * However, if any other type of error happens we want to stop right where we
 * are so that userspace will call back with exact number of the bad inode and
 * we can send back an error code.
 *
 * Note that if the formatter tells us there's no space left in the buffer we
 * move the cursor forward and abort the walk.
 */
STATIC int
xfs_bulkstat_one_int(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	void			*data)
{
	struct xfs_bstat_chunk	*bc = data;
	struct xfs_icdinode	*dic;		/* dinode core info pointer */
	struct xfs_inode	*ip;		/* incore inode pointer */
	struct inode		*inode;
	struct xfs_bstat	*buf;		/* return buffer */
	int			error = 0;	/* error value */

	if (xfs_internal_inum(mp, ino)) {
		bc->breq->lastino = ino;
		return -EINVAL;
	}

	buf = kmem_zalloc(sizeof(*buf), KM_SLEEP | KM_MAYFAIL);
	if (!buf)
		return -ENOMEM;

	error = xfs_iget(mp, NULL, ino,
			 (XFS_IGET_DONTCACHE | XFS_IGET_UNTRUSTED),
			 XFS_ILOCK_SHARED, &ip);
	if (error == -ENOENT || error == -EINVAL)
		bc->breq->lastino = ino;
	if (error)
		goto out_free;

	ASSERT(ip != NULL);
	ASSERT(ip->i_imap.im_blkno != 0);
	inode = VFS_I(ip);

	dic = &ip->i_d;

	/* xfs_iget returns the following without needing
	 * further change.
	 */
	buf->bs_projid_lo = dic->di_projid_lo;
	buf->bs_projid_hi = dic->di_projid_hi;
	buf->bs_ino = ino;
	buf->bs_uid = dic->di_uid;
	buf->bs_gid = dic->di_gid;
	buf->bs_size = dic->di_size;

	buf->bs_nlink = inode->i_nlink;
	buf->bs_atime.tv_sec = inode->i_atime.tv_sec;
	buf->bs_atime.tv_nsec = inode->i_atime.tv_nsec;
	buf->bs_mtime.tv_sec = inode->i_mtime.tv_sec;
	buf->bs_mtime.tv_nsec = inode->i_mtime.tv_nsec;
	buf->bs_ctime.tv_sec = inode->i_ctime.tv_sec;
	buf->bs_ctime.tv_nsec = inode->i_ctime.tv_nsec;
	buf->bs_gen = inode->i_generation;
	buf->bs_mode = inode->i_mode;

	buf->bs_xflags = xfs_ip2xflags(ip);
	buf->bs_extsize = dic->di_extsize << mp->m_sb.sb_blocklog;
	buf->bs_extents = dic->di_nextents;
	memset(buf->bs_pad, 0, sizeof(buf->bs_pad));
	xfs_bulkstat_health(ip, buf);
	buf->bs_dmevmask = dic->di_dmevmask;
	buf->bs_dmstate = dic->di_dmstate;
	buf->bs_aextents = dic->di_anextents;
	buf->bs_forkoff = XFS_IFORK_BOFF(ip);

	if (dic->di_version == 3) {
		if (dic->di_flags2 & XFS_DIFLAG2_COWEXTSIZE)
			buf->bs_cowextsize = dic->di_cowextsize <<
					mp->m_sb.sb_blocklog;
	}

	switch (dic->di_format) {
	case XFS_DINODE_FMT_DEV:
		buf->bs_rdev = sysv_encode_dev(inode->i_rdev);
		buf->bs_blksize = BLKDEV_IOSIZE;
		buf->bs_blocks = 0;
		break;
	case XFS_DINODE_FMT_LOCAL:
		buf->bs_rdev = 0;
		buf->bs_blksize = mp->m_sb.sb_blocksize;
		buf->bs_blocks = 0;
		break;
	case XFS_DINODE_FMT_EXTENTS:
	case XFS_DINODE_FMT_BTREE:
		buf->bs_rdev = 0;
		buf->bs_blksize = mp->m_sb.sb_blocksize;
		buf->bs_blocks = dic->di_nblocks + ip->i_delayed_blks;
		break;
	}
	xfs_iunlock(ip, XFS_ILOCK_SHARED);
	xfs_irele(ip);

	error = bc->formatter(bc->breq, buf);
	switch (error) {
	case XFS_IBULK_BUFFER_FULL:
		error = XFS_IWALK_ABORT;
		/* fall through */
	case 0:
		bc->breq->lastino = ino;
		break;
	}
out_free:
	kmem_free(buf);
	return error;
}

/* Bulkstat a single inode. */
int
xfs_bulkstat_one(
	struct xfs_ibulk	*breq,
	bulkstat_one_fmt_pf	formatter)
{
	struct xfs_bstat_chunk	bc = {
		.formatter	= formatter,
		.breq		= breq,
	};
	int			error;

	breq->icount = 1;
	breq->ocount = 0;

	error = xfs_bulkstat_one_int(breq->mp, breq->lastino, &bc);

	/*
	 * If we reported one inode to userspace then we abort because we hit
	 * the end of the buffer.  Don't leak that back to userspace.
	 */
	if (error == XFS_IWALK_ABORT)
		error = 0;

	return error;
}

static int
xfs_bulkstat_iwalk(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	void			*data)
{
	int			error;

	error = xfs_bulkstat_one_int(mp, ino, data);
	/* bulkstat just skips over missing inodes */
	if (error == -ENOENT || error == -EINVAL)
		return 0;
	return error;
}

/* Return stat information in bulk (by-inode) for the filesystem. */
int
xfs_bulkstat(
	struct xfs_ibulk	*breq,
	bulkstat_one_fmt_pf	formatter)
{
	struct xfs_bstat_chunk	bc = {
		.formatter	= formatter,
		.breq		= breq,
	};
	int			error;

	breq->ocount = 0;

	/*
	 * Get the last inode value, see if there's nothing to do.
	 */
	if (breq->lastino && !xfs_verify_ino(breq->mp, breq->lastino))
		return 0;

	error = xfs_iwalk(breq->mp, breq->lastino ? breq->lastino + 1 : 0,
			xfs_bulkstat_iwalk, &bc);

	/*
	 * We found some inodes, so clear the error status and return them.
	 * The lastino pointer will point directly at the inode that triggered
	 * any error that occurred, so on the next call the error will be
	 * triggered again and propagated to userspace as there will be no
	 * formatted inodes in the buffer.
	 */
	if (breq->ocount > 0)
		error = 0;

	return error;
}

struct xfs_inumbers_chunk {
	inumbers_fmt_pf		formatter;
	struct xfs_ibulk	*breq;
};

/*
 * Format the inode group structure and report it somewhere.
 *
 * Similar to xfs_bulkstat_one_int, lastino is the inode cursor as we walk
 * through the filesystem so we move it forward unless there was a runtime
 * error.  If the formatter tells us the buffer is now full we also move the
 * cursor forward and abort the walk.
 */
STATIC int
xfs_inumbers_walk(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	const struct xfs_inobt_rec_incore *irec,
	void			*data)
{
	struct xfs_inogrp	inogrp = {
		.xi_startino	= XFS_AGINO_TO_INO(mp, agno, irec->ir_startino),
		.xi_alloccount	= irec->ir_count - irec->ir_freecount,
		.xi_allocmask	= ~irec->ir_free,
	};
	struct xfs_inumbers_chunk *ic = data;
	xfs_agino_t		agino;
	int			error;

	error = ic->formatter(ic->breq, &inogrp);
	if (error && error != XFS_IBULK_BUFFER_FULL)
		return error;
	if (error == XFS_IBULK_BUFFER_FULL)
		error = XFS_INOBT_WALK_ABORT;

	agino = irec->ir_startino + XFS_INODES_PER_CHUNK - 1;
	ic->breq->lastino = XFS_AGINO_TO_INO(mp, agno, agino);
	return error;
}

/*
 * Return inode number table for the filesystem.
 */
int
xfs_inumbers(
	struct xfs_ibulk	*breq,
	inumbers_fmt_pf		formatter)
{
	struct xfs_inumbers_chunk ic = {
		.formatter	= formatter,
		.breq		= breq,
	};
	int			error = 0;

	breq->ocount = 0;

	/*
	 * Get the last inode value, see if there's nothing to do.
	 */
	if (breq->lastino && !xfs_verify_ino(breq->mp, breq->lastino))
		return 0;

	error = xfs_inobt_walk(breq->mp, breq->lastino ? breq->lastino + 1 : 0,
			xfs_inumbers_walk, &ic);

	/*
	 * We found some inode groups, so clear the error status and return
	 * them.  The lastino pointer will point directly at the inode that
	 * triggered any error that occurred, so on the next call the error
	 * will be triggered again and propagated to userspace as there will be
	 * no formatted inode groups in the buffer.
	 */
	if (breq->ocount > 0)
		error = 0;

	return error;
}
