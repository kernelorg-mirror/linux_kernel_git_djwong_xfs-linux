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

static inline struct xmbuf *to_xmbuf(struct xfs_buftarg *btp)
{
	return container_of(btp, struct xmbuf, target);
}

/*
 * shmem files used to back an in-memory buffer cache must not be exposed to
 * userspace.  Upper layers must coordinate access to the one handle returned
 * by the constructor, so establish a separate lock class for xmbufs to avoid
 * confusing lockdep.
 */
static struct lock_class_key xmbuf_i_mutex_key;

/* Close the file and release all resources. */
static void
xmbuf_close(
	struct xmbuf		*xmb)
{
	struct inode		*inode = file_inode(xmb->file);

	lockdep_set_class(&inode->i_rwsem, &inode->i_sb->s_type->i_mutex_key);
	fput(xmb->file);
	xmb->file = NULL;
}

/*
 * Allocate a buffer cache target for a memory-backed file and set up the
 * buffer target.
 */
int
xmbuf_alloc(
	struct xfs_mount	*mp,
	const char		*descr,
	struct xmbuf		**xmbp)
{
	struct file		*file;
	struct inode		*inode;
	struct xmbuf		*xmb;
	int			error;

	xmb = kzalloc(sizeof(*xmb), GFP_KERNEL);
	if (!xmb)
		return -ENOMEM;

	file = shmem_kernel_file_setup(descr, 0, 0);
	if (IS_ERR(file)) {
		error = PTR_ERR(file);
		goto out_xmb;
	}
	xmb->file = file;
	inode = file_inode(xmb->file);

	/* private file, private locking */
	lockdep_set_class(&inode->i_rwsem, &xmbuf_i_mutex_key);

	/* no highmem so we can use page_address */
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);

	/* all writes are below EOF */
	i_size_write(inode, inode->i_sb->s_maxbytes);

	trace_xmbuf_create(xmb);

	error = xfs_buf_cache_init(&xmb->bcache);
	if (error)
		goto out_file;

	/* Initialize buffer target */
	xmb->target.bt_mount = mp;
	xmb->target.bt_dev = (dev_t)-1U;
	xmb->target.bt_file = xmb->file;
	xmb->target.bt_flags |= XFS_BUFTARG_MEM;
	xmb->target.bt_cache = &xmb->bcache;

	xmb->target.bt_meta_sectorsize = XMBUF_BLOCKSIZE;
	xmb->target.bt_meta_sectormask = XMBUF_BLOCKSIZE - 1;
	xmb->target.bt_logical_sectorsize = XMBUF_BLOCKSIZE;
	xmb->target.bt_logical_sectormask = XMBUF_BLOCKSIZE - 1;

	/*
	 * Buffer IO error rate limiting. Limit it to no more than 10 messages
	 * per 30 seconds so as to not spam logs too much on repeated errors.
	 */
	ratelimit_state_init(&xmb->target.bt_ioerror_rl, 30 * HZ,
			     DEFAULT_RATELIMIT_BURST);

	error = list_lru_init(&xmb->target.bt_lru);
	if (error)
		goto out_bcache;

	error = percpu_counter_init(&xmb->target.bt_io_count, 0, GFP_KERNEL);
	if (error)
		goto out_lru;

	*xmbp = xmb;
	return 0;

out_lru:
	list_lru_destroy(&xmb->target.bt_lru);
out_bcache:
	xfs_buf_cache_destroy(&xmb->bcache);
out_file:
	xmbuf_close(xmb);
out_xmb:
	kfree(xmb);
	return error;
}

/* Free a buffer cache target for a memory-backed buffer cache. */
void
xmbuf_free(
	struct xmbuf		*xmb)
{
	struct xfs_buftarg	*btp = &xmb->target;

	ASSERT(btp->bt_flags & XFS_BUFTARG_MEM);
	ASSERT(percpu_counter_sum(&btp->bt_io_count) == 0);

	percpu_counter_destroy(&btp->bt_io_count);
	list_lru_destroy(&btp->bt_lru);

	xfs_buf_cache_destroy(&xmb->bcache);
	xmbuf_close(xmb);
	kfree(xmb);
}

/* Has this file lost any of the data stored in it? */
static inline bool
xmbuf_has_lost_data(
	struct inode		*inode,
	struct folio		*folio)
{
	struct address_space	*mapping = inode->i_mapping;

	/* This folio itself has been poisoned. */
	if (folio_test_hwpoison(folio))
		return true;

	/* A base page under this large folio has been poisoned. */
	if (folio_test_large(folio) && folio_test_has_hwpoisoned(folio))
		return true;

	/* Data loss has occurred anywhere in this shmem file. */
	if (test_bit(AS_EIO, &mapping->flags))
		return true;
	if (filemap_check_wb_err(mapping, 0))
		return true;

	return false;
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

	ASSERT(bp->b_target->bt_flags & XFS_BUFTARG_MEM);

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
	if (!folio)
		return -ENOMEM;

	if (xmbuf_has_lost_data(inode, folio)) {
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

	ASSERT(bp->b_target->bt_flags & XFS_BUFTARG_MEM);

	put_page(page);

	bp->b_addr = NULL;
	bp->b_pages[0] = NULL;
	bp->b_pages = NULL;
	bp->b_page_count = 0;
}
