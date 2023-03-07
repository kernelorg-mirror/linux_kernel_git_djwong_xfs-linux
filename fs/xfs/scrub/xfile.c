// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "scrub/xfile.h"
#include "scrub/xfarray.h"
#include "scrub/scrub.h"
#include "scrub/trace.h"
#include <linux/shmem_fs.h>

/*
 * Swappable Temporary Memory
 * ==========================
 *
 * Online checking sometimes needs to be able to stage a large amount of data
 * in memory.  This information might not fit in the available memory and it
 * doesn't all need to be accessible at all times.  In other words, we want an
 * indexed data buffer to store data that can be paged out.
 *
 * When CONFIG_TMPFS=y, shmemfs is enough of a filesystem to meet those
 * requirements.  Therefore, the xfile mechanism uses an unlinked shmem file to
 * store our staging data.  This file is not installed in the file descriptor
 * table so that user programs cannot access the data, which means that the
 * xfile must be freed with xfile_destroy.
 *
 * xfiles assume that the caller will handle all required concurrency
 * management; standard vfs locks (freezer and inode) are not taken.  Reads
 * and writes are satisfied directly from the page cache.
 *
 * NOTE: The current shmemfs implementation has a quirk that in-kernel reads
 * of a hole cause a page to be mapped into the file.  If you are going to
 * create a sparse xfile, please be careful about reading from uninitialized
 * parts of the file.  These pages are !Uptodate and will eventually be
 * reclaimed if not written, but in the short term this boosts memory
 * consumption.
 */

/*
 * xfiles must not be exposed to userspace and require upper layers to
 * coordinate access to the one handle returned by the constructor, so
 * establish a separate lock class for xfiles to avoid confusing lockdep.
 */
static struct lock_class_key xfile_i_mutex_key;

/*
 * Create an xfile of the given size.  The description will be used in the
 * trace output.
 */
int
xfile_create(
	const char		*description,
	loff_t			isize,
	struct xfile		**xfilep)
{
	struct inode		*inode;
	struct xfile		*xf;
	int			error = -ENOMEM;

	xf = kzalloc(sizeof(struct xfile), XCHK_GFP_FLAGS);
	if (!xf)
		return -ENOMEM;

	xf->file = shmem_file_setup(description, isize, 0);
	if (!xf->file)
		goto out_xfile;
	if (IS_ERR(xf->file)) {
		error = PTR_ERR(xf->file);
		goto out_xfile;
	}

	/*
	 * We want a large sparse file that we can pread, pwrite, and seek.
	 * xfile users are responsible for keeping the xfile hidden away from
	 * all other callers, so we skip timestamp updates and security checks.
	 * Make the inode only accessible by root, just in case the xfile ever
	 * escapes.
	 */
	xf->file->f_mode |= FMODE_PREAD | FMODE_PWRITE | FMODE_NOCMTIME |
			    FMODE_LSEEK;
	xf->file->f_flags |= O_RDWR | O_LARGEFILE | O_NOATIME;
	inode = file_inode(xf->file);
	inode->i_flags |= S_PRIVATE | S_NOCMTIME | S_NOATIME;
	inode->i_mode &= ~0177;
	inode->i_uid = GLOBAL_ROOT_UID;
	inode->i_gid = GLOBAL_ROOT_GID;

	lockdep_set_class(&inode->i_rwsem, &xfile_i_mutex_key);

	trace_xfile_create(xf);

	*xfilep = xf;
	return 0;
out_xfile:
	kfree(xf);
	return error;
}

/* Evict a cache entry and release the page. */
static inline int
xfile_cache_evict(
	struct xfile		*xf,
	struct xfile_cache	*entry)
{
	int			error;

	if (!entry->xfpage.page)
		return 0;

	lock_page(entry->xfpage.page);
	kunmap(entry->kaddr);

	error = xfile_put_page(xf, &entry->xfpage);
	memset(entry, 0, sizeof(struct xfile_cache));
	return error;
}

/*
 * Grab a page, map it into the kernel address space, and fill out the cache
 * entry.
 */
static int
xfile_cache_fill(
	struct xfile		*xf,
	loff_t			key,
	struct xfile_cache	*entry)
{
	int			error;

	error = xfile_get_page(xf, key, PAGE_SIZE, &entry->xfpage);
	if (error)
		return error;

	entry->kaddr = kmap(entry->xfpage.page);
	unlock_page(entry->xfpage.page);
	return 0;
}

/*
 * Return the kernel address of a cached position in the xfile.  If the cache
 * misses, the relevant page will be brought into memory, mapped, and returned.
 * If the cache is disabled, returns NULL.
 */
static void *
xfile_cache_lookup(
	struct xfile		*xf,
	loff_t			pos)
{
	loff_t			key = round_down(pos, PAGE_SIZE);
	unsigned int		i;
	int			ret;

	if (!(xf->flags & XFILE_INTERNAL_CACHE))
		return NULL;

	/* Is it already in the cache? */
	for (i = 0; i < XFILE_CACHE_ENTRIES; i++) {
		if (!xf->cached[i].xfpage.page)
			continue;
		if (page_offset(xf->cached[i].xfpage.page) != key)
			continue;

		goto found;
	}

	/* Find the least-used slot here so we can evict it. */
	for (i = 0; i < XFILE_CACHE_ENTRIES; i++) {
		if (!xf->cached[i].xfpage.page)
			goto insert;
	}
	i = min_t(unsigned int, i, XFILE_CACHE_ENTRIES - 1);

	ret = xfile_cache_evict(xf, &xf->cached[i]);
	if (ret)
		return ERR_PTR(ret);

insert:
	ret = xfile_cache_fill(xf, key, &xf->cached[i]);
	if (ret)
		return ERR_PTR(ret);

found:
	/* Stupid MRU moves this cache entry to the front. */
	if (i != 0)
		swap(xf->cached[0], xf->cached[i]);

	return xf->cached[0].kaddr;
}

/* Drop all cached xfile pages. */
static void
xfile_cache_drop(
	struct xfile		*xf)
{
	unsigned int		i;

	if (!(xf->flags & XFILE_INTERNAL_CACHE))
		return;

	for (i = 0; i < XFILE_CACHE_ENTRIES; i++)
		xfile_cache_evict(xf, &xf->cached[i]);
}

/* Enable the internal xfile cache. */
void
xfile_cache_enable(
	struct xfile		*xf)
{
	xf->flags |= XFILE_INTERNAL_CACHE;
	memset(xf->cached, 0, sizeof(struct xfile_cache) * XFILE_CACHE_ENTRIES);
}

/* Disable the internal xfile cache. */
void
xfile_cache_disable(
	struct xfile		*xf)
{
	xfile_cache_drop(xf);
	xf->flags &= ~XFILE_INTERNAL_CACHE;
}

/* Close the file and release all resources. */
void
xfile_destroy(
	struct xfile		*xf)
{
	struct inode		*inode = file_inode(xf->file);

	trace_xfile_destroy(xf);

	xfile_cache_drop(xf);

	lockdep_set_class(&inode->i_rwsem, &inode->i_sb->s_type->i_mutex_key);
	fput(xf->file);
	kfree(xf);
}

/* Get a mapped page in the xfile, do not use internal cache. */
static void *
xfile_uncached_get(
	struct xfile		*xf,
	loff_t			pos,
	struct xfile_page	*xfpage)
{
	loff_t			key = round_down(pos, PAGE_SIZE);
	int			error;

	error = xfile_get_page(xf, key, PAGE_SIZE, xfpage);
	if (error)
		return ERR_PTR(error);

	return kmap_local_page(xfpage->page);
}

/* Release a mapped page that was obtained via xfile_uncached_get. */
static int
xfile_uncached_put(
	struct xfile		*xf,
	struct xfile_page	*xfpage,
	void			*kaddr)
{
	kunmap_local(kaddr);
	return xfile_put_page(xf, xfpage);
}

/*
 * Read a memory object directly from the xfile's page cache.  Unlike regular
 * pread, we return -E2BIG and -EFBIG for reads that are too large or at too
 * high an offset, instead of truncating the read.  Otherwise, we return
 * bytes read or an error code, like regular pread.
 */
ssize_t
xfile_pread(
	struct xfile		*xf,
	void			*buf,
	size_t			count,
	loff_t			pos)
{
	struct inode		*inode = file_inode(xf->file);
	ssize_t			read = 0;
	unsigned int		pflags;
	int			error = 0;

	if (count > MAX_RW_COUNT)
		return -E2BIG;
	if (inode->i_sb->s_maxbytes - pos < count)
		return -EFBIG;

	trace_xfile_pread(xf, pos, count);

	pflags = memalloc_nofs_save();
	while (count > 0) {
		struct xfile_page xfpage;
		void		*p, *kaddr;
		unsigned int	len;
		bool		cached = true;

		len = min_t(ssize_t, count, PAGE_SIZE - offset_in_page(pos));

		kaddr = xfile_cache_lookup(xf, pos);
		if (!kaddr) {
			cached = false;
			kaddr = xfile_uncached_get(xf, pos, &xfpage);
		}
		if (IS_ERR(kaddr)) {
			error = PTR_ERR(kaddr);
			break;
		}

		p = kaddr + offset_in_page(pos);
		memcpy(buf, p, len);

		if (!cached) {
			error = xfile_uncached_put(xf, &xfpage, kaddr);
			if (error)
				break;
		}

		count -= len;
		pos += len;
		buf += len;
		read += len;
	}
	memalloc_nofs_restore(pflags);

	if (read > 0)
		return read;
	return error;
}

/*
 * Write a memory object directly to the xfile's page cache.  Unlike regular
 * pwrite, we return -E2BIG and -EFBIG for writes that are too large or at too
 * high an offset, instead of truncating the write.  Otherwise, we return
 * bytes written or an error code, like regular pwrite.
 */
ssize_t
xfile_pwrite(
	struct xfile		*xf,
	const void		*buf,
	size_t			count,
	loff_t			pos)
{
	struct inode		*inode = file_inode(xf->file);
	ssize_t			written = 0;
	unsigned int		pflags;
	int			error = 0;

	if (count > MAX_RW_COUNT)
		return -E2BIG;
	if (inode->i_sb->s_maxbytes - pos < count)
		return -EFBIG;

	trace_xfile_pwrite(xf, pos, count);

	pflags = memalloc_nofs_save();
	while (count > 0) {
		struct xfile_page xfpage;
		void		*p, *kaddr;
		unsigned int	len;
		bool		cached = true;

		len = min_t(ssize_t, count, PAGE_SIZE - offset_in_page(pos));

		kaddr = xfile_cache_lookup(xf, pos);
		if (!kaddr) {
			cached = false;
			kaddr = xfile_uncached_get(xf, pos, &xfpage);
		}
		if (IS_ERR(kaddr)) {
			error = PTR_ERR(kaddr);
			break;
		}

		p = kaddr + offset_in_page(pos);
		memcpy(p, buf, len);

		if (!cached) {
			error = xfile_uncached_put(xf, &xfpage, kaddr);
			if (error)
				break;
		}

		written += len;
		count -= len;
		pos += len;
		buf += len;
	}
	memalloc_nofs_restore(pflags);

	if (written > 0)
		return written;
	return error;
}

/* Discard pages backing a range of the xfile. */
void
xfile_discard(
	struct xfile		*xf,
	loff_t			pos,
	u64			count)
{
	trace_xfile_discard(xf, pos, count);
	xfile_cache_drop(xf);
	shmem_truncate_range(file_inode(xf->file), pos, pos + count - 1);
}

/* Ensure that there is storage backing the given range. */
int
xfile_prealloc(
	struct xfile		*xf,
	loff_t			pos,
	u64			count)
{
	struct inode		*inode = file_inode(xf->file);
	unsigned int		pflags;
	int			error = 0;

	if (count > MAX_RW_COUNT)
		return -E2BIG;
	if (inode->i_sb->s_maxbytes - pos < count)
		return -EFBIG;

	trace_xfile_prealloc(xf, pos, count);

	pflags = memalloc_nofs_save();
	while (count > 0) {
		struct xfile_page xfpage;
		void		*kaddr;
		unsigned int	len;

		len = min_t(ssize_t, count, PAGE_SIZE - offset_in_page(pos));

		kaddr = xfile_uncached_get(xf, pos, &xfpage);
		if (IS_ERR(kaddr)) {
			error = PTR_ERR(kaddr);
			break;
		}

		error = xfile_uncached_put(xf, &xfpage, kaddr);
		if (error)
			break;

		count -= len;
		pos += len;
	}
	memalloc_nofs_restore(pflags);

	return error;
}

/* Find the next written area in the xfile data for a given offset. */
loff_t
xfile_seek_data(
	struct xfile		*xf,
	loff_t			pos)
{
	loff_t			ret;

	ret = vfs_llseek(xf->file, pos, SEEK_DATA);
	trace_xfile_seek_data(xf, pos, ret);
	return ret;
}

/* Query stat information for an xfile. */
int
xfile_stat(
	struct xfile		*xf,
	struct xfile_stat	*statbuf)
{
	struct kstat		ks;
	int			error;

	error = vfs_getattr_nosec(&xf->file->f_path, &ks,
			STATX_SIZE | STATX_BLOCKS, AT_STATX_DONT_SYNC);
	if (error)
		return error;

	statbuf->size = ks.size;
	statbuf->bytes = ks.blocks << SECTOR_SHIFT;
	return 0;
}

/*
 * Grab the (locked) page for a memory object.  The object cannot span a page
 * boundary.  Returns 0 (and a locked page) if successful, -ENOTBLK if we
 * cannot grab the page, or the usual negative errno.
 */
int
xfile_get_page(
	struct xfile		*xf,
	loff_t			pos,
	unsigned int		len,
	struct xfile_page	*xfpage)
{
	struct inode		*inode = file_inode(xf->file);
	struct address_space	*mapping = inode->i_mapping;
	const struct address_space_operations *aops = mapping->a_ops;
	struct page		*page = NULL;
	void			*fsdata = NULL;
	loff_t			key = round_down(pos, PAGE_SIZE);
	unsigned int		pflags;
	int			error;

	if (inode->i_sb->s_maxbytes - pos < len)
		return -ENOMEM;
	if (len > PAGE_SIZE - offset_in_page(pos))
		return -ENOTBLK;

	trace_xfile_get_page(xf, pos, len);

	pflags = memalloc_nofs_save();

	/*
	 * We call write_begin directly here to avoid all the freezer
	 * protection lock-taking that happens in the normal path.  shmem
	 * doesn't support fs freeze, but lockdep doesn't know that and will
	 * trip over that.
	 */
	error = aops->write_begin(NULL, mapping, key, PAGE_SIZE, &page,
			&fsdata);
	if (error)
		goto out_pflags;

	/* We got the page, so make sure we push out EOF. */
	if (i_size_read(inode) < pos + len)
		i_size_write(inode, pos + len);

	/*
	 * If the page isn't up to date, fill it with zeroes before we hand it
	 * to the caller and make sure the backing store will hold on to them.
	 */
	if (!PageUptodate(page)) {
		void	*kaddr;

		kaddr = kmap_local_page(page);
		memset(kaddr, 0, PAGE_SIZE);
		kunmap_local(kaddr);
		SetPageUptodate(page);
	}

	/*
	 * Mark each page dirty so that the contents are written to some
	 * backing store when we drop this buffer, and take an extra reference
	 * to prevent the xfile page from being swapped or removed from the
	 * page cache by reclaim if the caller unlocks the page.
	 */
	set_page_dirty(page);
	get_page(page);

	xfpage->page = page;
	xfpage->fsdata = fsdata;
	xfpage->pos = key;
out_pflags:
	memalloc_nofs_restore(pflags);
	return error;
}

/*
 * Release the (locked) page for a memory object.  Returns 0 or a negative
 * errno.
 */
int
xfile_put_page(
	struct xfile		*xf,
	struct xfile_page	*xfpage)
{
	struct inode		*inode = file_inode(xf->file);
	struct address_space	*mapping = inode->i_mapping;
	const struct address_space_operations *aops = mapping->a_ops;
	unsigned int		pflags;
	int			ret;

	trace_xfile_put_page(xf, xfpage->pos, xfpage->page);

	/* Give back the reference that we took in xfile_get_page. */
	put_page(xfpage->page);

	pflags = memalloc_nofs_save();
	ret = aops->write_end(NULL, mapping, xfpage->pos, PAGE_SIZE, PAGE_SIZE,
			xfpage->page, xfpage->fsdata);
	memalloc_nofs_restore(pflags);
	memset(xfpage, 0, sizeof(struct xfile_page));

	if (ret < 0)
		return ret;
	if (ret != PAGE_SIZE)
		return -EIO;
	return 0;
}

/* Dump an xfile to dmesg. */
int
xfile_dump(
	struct xfile		*xf)
{
	struct xfile_stat	sb;
	struct inode		*inode = file_inode(xf->file);
	struct address_space	*mapping = inode->i_mapping;
	loff_t			holepos = 0;
	loff_t			datapos;
	loff_t			ret;
	unsigned int		pflags;
	bool			all_zeroes = true;
	int			error = 0;

	error = xfile_stat(xf, &sb);
	if (error)
		return error;

	printk(KERN_ALERT "xfile ino 0x%lx isize 0x%llx dump:", inode->i_ino,
			sb.size);
	pflags = memalloc_nofs_save();

	while ((ret = vfs_llseek(xf->file, holepos, SEEK_DATA)) >= 0) {
		datapos = rounddown_64(ret, PAGE_SIZE);
		ret = vfs_llseek(xf->file, datapos, SEEK_HOLE);
		if (ret < 0)
			break;
		holepos = min_t(loff_t, sb.size, roundup_64(ret, PAGE_SIZE));

		while (datapos < holepos) {
			struct page	*page = NULL;
			void		*p, *kaddr;
			u64		datalen = holepos - datapos;
			unsigned int	pagepos;
			unsigned int	pagelen;

			cond_resched();

			if (fatal_signal_pending(current)) {
				error = -EINTR;
				goto out_pflags;
			}

			pagelen = min_t(u64, datalen, PAGE_SIZE);

			page = shmem_read_mapping_page_gfp(mapping,
					datapos >> PAGE_SHIFT, __GFP_NOWARN);
			if (IS_ERR(page)) {
				error = PTR_ERR(page);
				if (error == -EIO)
					printk(KERN_ALERT "%.8llx: poisoned",
							datapos);
				else if (error != -ENOMEM)
					goto out_pflags;

				goto next_pgoff;
			}

			if (!PageUptodate(page))
				goto next_page;

			kaddr = kmap_local_page(page);
			p = kaddr;

			for (pagepos = 0; pagepos < pagelen; pagepos += 16) {
				char prefix[16];
				unsigned int linelen;

				linelen = min_t(unsigned int, pagelen, 16);

				if (!memchr_inv(p + pagepos, 0, linelen))
					continue;

				snprintf(prefix, 16, "%.8llx: ",
						datapos + pagepos);

				all_zeroes = false;
				print_hex_dump(KERN_ALERT, prefix,
						DUMP_PREFIX_NONE, 16, 1,
						p + pagepos, linelen, true);
			}
			kunmap_local(kaddr);
next_page:
			put_page(page);
next_pgoff:
			datapos += PAGE_SIZE;
		}
	}
	if (all_zeroes)
		printk(KERN_ALERT "<all zeroes>");
	if (ret != -ENXIO)
		error = ret;
out_pflags:
	memalloc_nofs_restore(pflags);
	return error;
}
