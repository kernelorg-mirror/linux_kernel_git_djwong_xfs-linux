// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_buf.h"
#include "xfs_buf_mem.h"
#include "xfs_trace.h"
#include <linux/shmem_fs.h>
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_buf_item.h"
#include "xfs_error.h"

struct kmem_cache	*xfs_self_buftarg_cache;

/*
 * Buffer Cache for In-Memory Files
 * ================================
 *
 * Online fsck wants to create ephemeral ordered recordsets.  The existing
 * btree infrastructure can do this, but we need the buffer cache to target
 * memory instead of block devices.
 *
 * When CONFIG_TMPFS=y, shmemfs is enough of a filesystem to meet those
 * requirements.  Therefore, the xmbuf mechanism uses an unlinked shmem file to
 * store our staging data.  This file is not installed in the file descriptor
 * table so that user programs cannot access the data, which means that the
 * xmbuf must be freed with xmbuf_destroy.
 *
 * xmbufs assume that the caller will handle all required concurrency
 * management; standard vfs locks (freezer and inode) are not taken.  Reads
 * and writes are satisfied directly from the page cache.
 *
 * The only supported block size is PAGE_SIZE, and we cannot use highmem.
 */

/*
 * shmem files used to back an in-memory buffer cache must not be exposed to
 * userspace.  Upper layers must coordinate access to the one handle returned
 * by the constructor, so establish a separate lock class for xmbufs to avoid
 * confusing lockdep.
 */
static struct lock_class_key xmbuf_i_mutex_key;

/*
 * Allocate a buffer cache target for a memory-backed file and set up the
 * buffer target.
 */
int
xmbuf_alloc(
	struct xfs_mount	*mp,
	const char		*descr,
	struct xfs_buftarg	**btpp)
{
	struct file		*file;
	struct inode		*inode;
	struct xfs_buftarg	*btp;
	int			error;

	btp = kmem_cache_zalloc(xfs_self_buftarg_cache, GFP_KERNEL);
	if (!btp)
		return -ENOMEM;

	file = shmem_kernel_file_setup(descr, 0, 0);
	if (IS_ERR(file)) {
		error = PTR_ERR(file);
		goto out_free_btp;
	}
	inode = file_inode(file);

	/* private file, private locking */
	lockdep_set_class(&inode->i_rwsem, &xmbuf_i_mutex_key);

	/*
	 * We don't want to bother with kmapping data during repair, so don't
	 * allow highmem pages to back this mapping.
	 */
	mapping_set_gfp_mask(inode->i_mapping, GFP_KERNEL);

	/* ensure all writes are below EOF to avoid pagecache zeroing */
	i_size_write(inode, inode->i_sb->s_maxbytes);

	error = xfs_buf_cache_init(btp->bt_cache);
	if (error)
		goto out_file;

	/* Initialize buffer target */
	btp->bt_mount = mp;
	btp->bt_dev = (dev_t)-1U;
	btp->bt_bdev = NULL; /* in-memory buftargs have no bdev */
	btp->bt_file = file;
	btp->bt_meta_sectorsize = XMBUF_BLOCKSIZE;
	btp->bt_meta_sectormask = XMBUF_BLOCKSIZE - 1;

	error = xfs_init_buftarg(btp, XMBUF_BLOCKSIZE, descr);
	if (error)
		goto out_bcache;

	trace_xmbuf_create(btp);

	*btpp = btp;
	return 0;

out_bcache:
	xfs_buf_cache_destroy(btp->bt_cache);
out_file:
	fput(file);
out_free_btp:
	kmem_cache_free(xfs_self_buftarg_cache, btp);
	return error;
}

/* Free a buffer cache target for a memory-backed buffer cache. */
void
xmbuf_free(
	struct xfs_buftarg	*btp)
{
	ASSERT(xfs_buftarg_is_mem(btp));
	ASSERT(percpu_counter_sum(&btp->bt_io_count) == 0);

	trace_xmbuf_free(btp);

	xfs_destroy_buftarg(btp);
	xfs_buf_cache_destroy(btp->bt_cache);
	fput(btp->bt_file);
	kmem_cache_free(xfs_self_buftarg_cache, btp);
}

/* Directly map a shmem page into the buffer cache. */
int
xmbuf_map_page(
	struct xfs_buf		*bp)
{
	struct inode		*inode = file_inode(bp->b_target->bt_file);
	struct folio		*folio = NULL;
	struct page		*page;
	loff_t                  pos = BBTOB(xfs_buf_daddr(bp));
	int			error;

	ASSERT(xfs_buftarg_is_mem(bp->b_target));

	if (bp->b_map_count != 1)
		return -ENOMEM;
	if (BBTOB(bp->b_length) != XMBUF_BLOCKSIZE)
		return -ENOMEM;
	if (offset_in_page(pos) != 0) {
		ASSERT(offset_in_page(pos));
		return -ENOMEM;
	}

	error = shmem_get_folio(inode, pos >> PAGE_SHIFT, &folio, SGP_CACHE);
	if (error)
		return error;

	if (filemap_check_wb_err(inode->i_mapping, 0)) {
		folio_unlock(folio);
		folio_put(folio);
		return -EIO;
	}

	page = folio_file_page(folio, pos >> PAGE_SHIFT);

	/*
	 * Mark the page dirty so that it won't be reclaimed once we drop the
	 * (potentially last) reference in xmbuf_unmap_page.
	 */
	set_page_dirty(page);
	unlock_page(page);

	bp->b_addr = page_address(page);
	bp->b_pages = bp->b_page_array;
	bp->b_pages[0] = page;
	bp->b_page_count = 1;
	return 0;
}

/* Unmap a shmem page that was mapped into the buffer cache. */
void
xmbuf_unmap_page(
	struct xfs_buf		*bp)
{
	struct page		*page = bp->b_pages[0];

	ASSERT(xfs_buftarg_is_mem(bp->b_target));

	put_page(page);

	bp->b_addr = NULL;
	bp->b_pages[0] = NULL;
	bp->b_pages = NULL;
	bp->b_page_count = 0;
}

/* Is this a valid daddr within the buftarg? */
bool
xmbuf_verify_daddr(
	struct xfs_buftarg	*btp,
	xfs_daddr_t		daddr)
{
	struct inode		*inode = file_inode(btp->bt_file);

	ASSERT(xfs_buftarg_is_mem(btp));

	return daddr < (inode->i_sb->s_maxbytes >> BBSHIFT);
}

/* Discard the page backing this buffer. */
static void
xmbuf_stale(
	struct xfs_buf		*bp)
{
	struct inode		*inode = file_inode(bp->b_target->bt_file);
	loff_t			pos;

	ASSERT(xfs_buftarg_is_mem(bp->b_target));

	pos = BBTOB(xfs_buf_daddr(bp));
	shmem_truncate_range(inode, pos, pos + BBTOB(bp->b_length) - 1);
}

/*
 * Finalize a buffer -- discard the backing page if it's stale, or run the
 * write verifier to detect problems.
 */
int
xmbuf_finalize(
	struct xfs_buf		*bp)
{
	xfs_failaddr_t		fa;
	int			error = 0;

	if (bp->b_flags & XBF_STALE) {
		xmbuf_stale(bp);
		return 0;
	}

	/*
	 * Although this btree is ephemeral, validate the buffer structure so
	 * that we can detect memory corruption errors and software bugs.
	 */
	fa = bp->b_ops->verify_struct(bp);
	if (fa) {
		error = -EFSCORRUPTED;
		xfs_verifier_error(bp, error, fa);
	}

	return error;
}

/*
 * Detach this xmbuf buffer from the transaction by any means necessary.
 * All buffers are direct-mapped, so they do not need bwrite.
 */
void
xmbuf_trans_bdetach(
	struct xfs_trans	*tp,
	struct xfs_buf		*bp)
{
	struct xfs_buf_log_item	*bli = bp->b_log_item;

	ASSERT(bli != NULL);

	bli->bli_flags &= ~(XFS_BLI_DIRTY | XFS_BLI_ORDERED |
			    XFS_BLI_LOGGED | XFS_BLI_STALE);
	clear_bit(XFS_LI_DIRTY, &bli->bli_item.li_flags);

	while (bp->b_log_item != NULL)
		xfs_trans_bdetach(tp, bp);
}

/*
 * Blob Cache
 * ==========
 *
 * Some callers would like to be able to cache arbitrary blobs and be able to
 * look them up via a numeric key.  Create self-caching buftargs and add some
 * bespoke functions to peek, get, and store blobs.
 */

/* Allocate a buffer cache target to index blobs. */
int
xblobc_alloc(
	struct xfs_mount	*mp,
	dev_t			cookie,
	const char		*descr,
	struct xfs_buftarg	**btpp)
{
	struct xfs_buftarg	*btp;
	int			error;

	btp = kmem_cache_zalloc(xfs_self_buftarg_cache, GFP_KERNEL);
	if (!btp)
		return -ENOMEM;

	error = xfs_buf_cache_init(btp->bt_cache);
	if (error)
		goto out_free_btp;

	/* Initialize buffer target */
	btp->bt_mount = mp;
	btp->bt_dev = cookie;
	btp->bt_bdev = NULL; /* blob caches have no bdev or file */
	btp->bt_file = NULL;
	btp->bt_meta_sectorsize = XBC_BLOCKSIZE;
	btp->bt_meta_sectormask = XBC_BLOCKSIZE - 1;

	error = xfs_init_buftarg(btp, XBC_BLOCKSIZE, descr);
	if (error)
		goto out_bcache;

	trace_xblobc_alloc(btp);

	*btpp = btp;
	return 0;

out_bcache:
	xfs_buf_cache_destroy(btp->bt_cache);
out_free_btp:
	kmem_cache_free(xfs_self_buftarg_cache, btp);
	return error;
}

/* Free a blob cache target for a memory-backed buffer cache. */
void
xblobc_free(
	struct xfs_buftarg	*btp)
{
	ASSERT(xfs_buftarg_is_blobcache(btp));
	ASSERT(percpu_counter_sum(&btp->bt_io_count) == 0);

	trace_xblobc_free(btp);

	xfs_buftarg_drain(btp);
	xfs_destroy_buftarg(btp);
	xfs_buf_cache_destroy(btp->bt_cache);
	kmem_cache_free(xfs_self_buftarg_cache, btp);
}

/*
 * Retrieve cached data from the blob cache.  Returns -ENODATA if nothing
 * was cached.  @bytecount must be a multiple of XBC_BLOCKSIZE.
 */
int
xblobc_peek(
	struct xfs_buftarg	*target,
	xblobc_key_t		key,
	size_t			bytecount,
	struct xfs_buf		**bpp)
{
	struct xfs_buf		*bp;
	int			error;

	ASSERT(xfs_buftarg_is_blobcache(target));

	error = xfs_buf_incore(target, key, BTOBB(bytecount), 0, &bp);
	if (error == -ENOENT)
		return -ENODATA;
	if (error)
		return error;

	trace_xblobc_peek(bp, _RET_IP_);

	if (!bp->b_addr) {
		xfs_buf_relse(bp);
		return -ENODATA;
	}

	*bpp = bp;
	return 0;
}

/* Retrieve the handle in preparation for storing a blob. */
int
xblobc_get(
	struct xfs_buftarg	*target,
	xblobc_key_t		key,
	size_t			bytecount,
	struct xfs_buf		**bpp)
{
	int			error;

	ASSERT(xfs_buftarg_is_blobcache(target));

	error = xfs_buf_get(target, key, BTOBB(bytecount), 0, bpp);
	if (error)
		return error;

	trace_xblobc_get(*bpp, _RET_IP_);

	return 0;
}

/* Store data into the cache.  The xfs_buf takes ownership of the data. */
void
xblobc_store(
	struct xfs_buf		*bp,
	void			*data,
	size_t			datalen)
{
	ASSERT(xfs_buftarg_is_blobcache(bp->b_target));
	ASSERT(BTOBB(datalen) == bp->b_length);
	ASSERT(!(bp->b_flags & _XBF_PAGES));

	trace_xblobc_store(bp, _RET_IP_);

	if (bp->b_addr) {
		ASSERT(bp->b_flags & _XBF_KMEM);
		ASSERT(bp->b_pages == bp->b_page_array);
		ASSERT(bp->b_pages[0] == NULL);
		ASSERT(bp->b_page_count == 0);

		kvfree(bp->b_addr);
	}

	bp->b_addr = data;
	bp->b_offset = 0;
	bp->b_pages = bp->b_page_array;
	bp->b_pages[0] = NULL;
	bp->b_page_count = 0;
	bp->b_flags |= (_XBF_KMEM | XBF_DONE);
}
