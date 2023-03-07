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

	if (bp->b_target->bt_flags & XFS_BUFTARG_DIRECT_MAP) {
		/* direct mapping means no io necessary */
		return 0;
	}

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

/* Sector count for this xfile buftarg. */
xfs_daddr_t
xfile_buftarg_nr_sectors(
	struct xfs_buftarg	*btp)
{
	return xfile_size(btp->bt_xfile) >> SECTOR_SHIFT;
}

/* Free an xfile page that was directly mapped into the buffer cache. */
static int
xfile_buf_put_page(
	struct xfile		*xfile,
	loff_t			pos,
	struct page		*page)
{
	struct xfile_page	xfpage = {
		.page		= page,
		.pos		= round_down(pos, PAGE_SIZE),
	};

	lock_page(xfpage.page);

	return xfile_put_page(xfile, &xfpage);
}

/* Grab the xfile page for this part of the xfile. */
static int
xfile_buf_get_page(
	struct xfile		*xfile,
	loff_t			pos,
	unsigned int		len,
	struct page		**pagep)
{
	struct xfile_page	xfpage = { NULL };
	int			error;

	error = xfile_get_page(xfile, pos, len, &xfpage);
	if (error)
		return error;

	/*
	 * Fall back to regular DRAM buffers if tmpfs gives us fsdata or the
	 * page pos isn't what we were expecting.
	 */
	if (xfpage.fsdata || xfpage.pos != round_down(pos, PAGE_SIZE)) {
		xfile_put_page(xfile, &xfpage);
		return -ENOTBLK;
	}

	/* Unlock the page before we start using them for the buffer cache. */
	ASSERT(PageUptodate(xfpage.page));
	unlock_page(xfpage.page);

	*pagep = xfpage.page;
	return 0;
}

/*
 * Try to map storage directly, if the target supports it.  Returns 0 for
 * success, -ENOTBLK to mean "not supported", or the usual negative errno.
 */
int
xfile_buf_map_pages(
	struct xfs_buf		*bp,
	xfs_buf_flags_t		flags)
{
	struct xfs_buf_map	*map;
	gfp_t			gfp_mask = __GFP_NOWARN;
	const unsigned int	page_align_mask = PAGE_SIZE - 1;
	unsigned int		m, p, n;
	int			error;

	ASSERT(xfile_buftarg_can_direct_map(bp->b_target));

	/* For direct-map buffers, each map has to be page aligned. */
	for (m = 0, map = bp->b_maps; m < bp->b_map_count; m++, map++)
		if (BBTOB(map->bm_bn | map->bm_len) & page_align_mask)
			return -ENOTBLK;

	if (flags & XBF_READ_AHEAD)
		gfp_mask |= __GFP_NORETRY;
	else
		gfp_mask |= GFP_NOFS;

	error = xfs_buf_alloc_page_array(bp, gfp_mask);
	if (error)
		return error;

	/* Map in the xfile pages. */
	for (m = 0, p = 0, map = bp->b_maps; m < bp->b_map_count; m++, map++) {
		for (n = 0; n < map->bm_len; n += BTOBB(PAGE_SIZE)) {
			unsigned int	len;

			len = min_t(unsigned int, BBTOB(map->bm_len - n),
					PAGE_SIZE);

			error = xfile_buf_get_page(bp->b_target->bt_xfile,
					BBTOB(map->bm_bn + n), len,
					&bp->b_pages[p++]);
			if (error)
				goto fail;
		}
	}

	bp->b_flags |= _XBF_DIRECT_MAP;
	return 0;

fail:
	/*
	 * Release all the xfile pages and free the page array, we're falling
	 * back to a DRAM buffer, which could be pages or a slab allocation.
	 */
	for (m = 0, p = 0, map = bp->b_maps; m < bp->b_map_count; m++, map++) {
		for (n = 0; n < map->bm_len; n += BTOBB(PAGE_SIZE)) {
			if (bp->b_pages[p] == NULL)
				continue;

			xfile_buf_put_page(bp->b_target->bt_xfile,
					BBTOB(map->bm_bn + n),
					bp->b_pages[p++]);
		}
	}

	xfs_buf_free_page_array(bp);
	return error;
}

/* Unmap all the direct-mapped buffer pages. */
void
xfile_buf_unmap_pages(
	struct xfs_buf		*bp)
{
	struct xfs_buf_map	*map;
	unsigned int		m, p, n;
	int			error = 0, err2;

	ASSERT(xfile_buftarg_can_direct_map(bp->b_target));

	for (m = 0, p = 0, map = bp->b_maps; m < bp->b_map_count; m++, map++) {
		for (n = 0; n < map->bm_len; n += BTOBB(PAGE_SIZE)) {
			err2 = xfile_buf_put_page(bp->b_target->bt_xfile,
					BBTOB(map->bm_bn + n),
					bp->b_pages[p++]);
			if (!error && err2)
				error = err2;
		}
	}

	if (error)
		xfs_err(bp->b_mount, "%s failed errno %d", __func__, error);

	bp->b_flags &= ~_XBF_DIRECT_MAP;
	xfs_buf_free_page_array(bp);
}
