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
 * requirements.  Therefore, the xfbuf mechanism uses an unlinked shmem file to
 * store our staging data.  This file is not installed in the file descriptor
 * table so that user programs cannot access the data, which means that the
 * xfbuf must be freed with xfbuf_destroy.
 *
 * xfbufs assume that the caller will handle all required concurrency
 * management; standard vfs locks (freezer and inode) are not taken.  Reads
 * and writes are satisfied directly from the page cache.
 *
 * The only supported block size is PAGE_SIZE, and we cannot use highmem.
 */

static inline struct xfbuf *to_xfbuf(struct xfs_buftarg *btp)
{
	return container_of(btp->bt_cache, struct xfbuf, bcache);
}

/*
 * shmem files used to back an in-memory buffer cache must not be exposed to
 * userspace.  Upper layers must coordinate access to the one handle returned
 * by the constructor, so establish a separate lock class for xfbufs to avoid
 * confusing lockdep.
 */
static struct lock_class_key xfbuf_i_mutex_key;

/* Close the file and release all resources. */
static void
xfbuf_close(
	struct xfbuf		*xfb)
{
	struct inode		*inode = file_inode(xfb->file);

	lockdep_set_class(&inode->i_rwsem, &inode->i_sb->s_type->i_mutex_key);
	fput(xfb->file);
	xfb->file = NULL;
}

/*
 * Allocate a buffer cache target for a memory-backed file and set up the
 * buffer target.
 */
int
xfbuf_alloc(
	struct xfs_mount	*mp,
	const char		*descr,
	struct xfbuf		**xfbp)
{
	struct xfs_buftarg	*btp;
	struct file		*file;
	struct inode		*inode;
	struct xfbuf		*xfb;
	int			error;

	xfb = kzalloc(sizeof(*xfb), GFP_KERNEL);
	if (!xfb)
		return -ENOMEM;

	file = shmem_kernel_file_setup(descr, 0, 0);
	if (IS_ERR(file)) {
		error = PTR_ERR(file);
		goto out_xfb;
	}

	xfb->file = file;
	inode = file_inode(xfb->file);

	/* private file, private locking */
	lockdep_set_class(&inode->i_rwsem, &xfbuf_i_mutex_key);

	/* no highmem so we can use page_address */
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);

	/* no large folios or THPs */
	mapping_clear_large_folios(inode->i_mapping);

	/* all writes are below EOF */
	i_size_write(inode, inode->i_sb->s_maxbytes);

	trace_xfbuf_create(xfb);

	error = xfs_buf_cache_init(&xfb->bcache);
	if (error)
		goto out_file;

	btp = xfs_alloc_buftarg_common(mp, descr);
	if (!btp) {
		error = -ENOMEM;
		goto out_bcache;
	}
	xfb->target = btp;

	btp->bt_file = xfb->file;
	btp->bt_dev = (dev_t)-1U;
	btp->bt_flags |= XFS_BUFTARG_MEM;
	btp->bt_cache = &xfb->bcache;

	btp->bt_meta_sectorsize = XFBUF_BLOCKSIZE;
	btp->bt_meta_sectormask = XFBUF_BLOCKSIZE - 1;
	btp->bt_logical_sectorsize = XFBUF_BLOCKSIZE;
	btp->bt_logical_sectormask = XFBUF_BLOCKSIZE - 1;

	*xfbp = xfb;
	return 0;

out_bcache:
	xfs_buf_cache_destroy(&xfb->bcache);
out_file:
	xfbuf_close(xfb);
out_xfb:
	kfree(xfb);
	return error;
}

/* Free a buffer cache target for a memory-backed buffer cache. */
void
xfbuf_free(
	struct xfbuf		*xfb)
{
	struct xfs_buftarg	*btp = xfb->target;

	ASSERT(btp->bt_flags & XFS_BUFTARG_MEM);

	xfs_free_buftarg(btp);
	xfs_buf_cache_destroy(&xfb->bcache);
	xfbuf_close(xfb);
	kfree(xfb);
}

/* Directly map a shmem page into the buffer cache. */
int
xfbuf_map_pages(
	struct xfs_buf		*bp,
	xfs_buf_flags_t		flags)
{
	struct address_space	*mapping = bp->b_target->bt_file->f_mapping;
	struct page		*page;
	gfp_t			gfp = __GFP_NOWARN;
	loff_t                  pos = BBTOB(xfs_buf_daddr(bp));

	ASSERT(bp->b_target->bt_flags & XFS_BUFTARG_MEM);

	if (bp->b_map_count != 1)
		return -ENOMEM;
	if (BBTOB(bp->b_length) != XFBUF_BLOCKSIZE)
		return -ENOMEM;
	if (offset_in_page(pos) != 0) {
		ASSERT(offset_in_page(pos));
		return -ENOMEM;
	}

	if (flags & XBF_READ_AHEAD)
		gfp |= __GFP_NORETRY;

	page = shmem_read_mapping_page_gfp(mapping, pos >> PAGE_SHIFT, gfp);
	if (IS_ERR(page))
		return PTR_ERR(page);

	bp->b_addr = page_address(page);
	bp->b_pages = bp->b_page_array;
	bp->b_pages[0] = page;
	bp->b_page_count = 1;
	return 0;
}

/* Unmap shmem page that were mapped into the buffer cache. */
void
xfbuf_unmap_pages(
	struct xfs_buf		*bp)
{
	struct page		*page = bp->b_pages[0];

	ASSERT(bp->b_target->bt_flags & XFS_BUFTARG_MEM);

	lock_page(page);
	set_page_dirty(page);
	unlock_page(page);
	put_page(page);

	bp->b_addr = NULL;
	bp->b_pages[0] = NULL;
	bp->b_pages = NULL;
	bp->b_page_count = 0;
}

/* Is this a valid daddr within the buftarg? */
bool
xfbuf_verify_daddr(
	struct xfs_buftarg	*btp,
	xfs_daddr_t		daddr)
{
	struct xfbuf		*xfb = to_xfbuf(btp);
	struct inode		*inode = file_inode(xfb->file);

	ASSERT(btp->bt_flags & XFS_BUFTARG_MEM);

	return daddr < (inode->i_sb->s_maxbytes >> BBSHIFT);
}
