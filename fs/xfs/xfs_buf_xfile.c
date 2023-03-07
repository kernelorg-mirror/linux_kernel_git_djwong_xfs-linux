// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_buf.h"
#include "xfs_buf_xfile.h"
#include "scrub/xfile.h"

/* Perform a buffer IO to an xfile.  Caller must be in process context. */
int
xfile_buf_ioapply(
	struct xfs_buf		*bp)
{
	struct xfile		*xfile = bp->b_target->bt_xfile;
	loff_t			pos = BBTOB(xfs_buf_daddr(bp));
	size_t			size = BBTOB(bp->b_length);

	if (bp->b_map_count > 1) {
		/* We don't need or support multi-map buffers. */
		ASSERT(0);
		return -EIO;
	}

	if (bp->b_flags & XBF_WRITE)
		return xfile_obj_store(xfile, bp->b_addr, size, pos);
	return xfile_obj_load(xfile, bp->b_addr, size, pos);
}

/* Allocate a buffer cache target for a memory-backed file. */
int
xfile_alloc_buftarg(
	struct xfs_mount	*mp,
	const char		*descr,
	struct xfs_buftarg	**btpp)
{
	struct xfs_buftarg	*btp;
	struct xfile		*xfile;
	int			error;

	error = xfile_create(descr, 0, &xfile);
	if (error)
		return error;

	error = xfs_buf_cache_init(&xfile->bcache);
	if (error)
		goto out_xfile;

	btp = xfs_alloc_buftarg_common(mp, descr);
	if (!btp) {
		error = -ENOMEM;
		goto out_bcache;
	}

	btp->bt_xfile = xfile;
	btp->bt_dev = (dev_t)-1U;
	btp->bt_flags |= XFS_BUFTARG_XFILE;
	btp->bt_cache = &xfile->bcache;

	btp->bt_meta_sectorsize = SECTOR_SIZE;
	btp->bt_meta_sectormask = SECTOR_SIZE - 1;
	btp->bt_logical_sectorsize = SECTOR_SIZE;
	btp->bt_logical_sectormask = SECTOR_SIZE - 1;

	*btpp = btp;
	return 0;

out_bcache:
	xfs_buf_cache_destroy(&xfile->bcache);
out_xfile:
	xfile_destroy(xfile);
	return error;
}

/* Free a buffer cache target for a memory-backed file. */
void
xfile_free_buftarg(
	struct xfs_buftarg	*btp)
{
	struct xfile		*xfile = btp->bt_xfile;

	ASSERT(btp->bt_flags & XFS_BUFTARG_XFILE);

	xfs_free_buftarg(btp);
	xfs_buf_cache_destroy(&xfile->bcache);
	xfile_destroy(xfile);
}
