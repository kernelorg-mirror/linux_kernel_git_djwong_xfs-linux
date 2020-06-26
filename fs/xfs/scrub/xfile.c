// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "scrub/array.h"
#include "scrub/scrub.h"
#include "scrub/trace.h"
#include "scrub/xfile.h"
#include <linux/shmem_fs.h>

/*
 * Create a memfd to our specifications and return a file pointer.  The file
 * is not installed in the file description table (because userspace has no
 * business accessing our internal data), which means that the caller /must/
 * call xfile_destroy when finished.
 */
struct xfile *
xfile_create(
	const char	*description,
	loff_t		size)
{
	struct xfile	*xf;

	xf = kmem_alloc(sizeof(struct xfile), KM_MAYFAIL);
	if (!xf)
		return ERR_PTR(-ENOMEM);

	xf->filp = shmem_file_setup(description, size, 0);
	if (!xf->filp) {
		kmem_free(xf);
		return ERR_PTR(-ENOMEM);
	}
	if (IS_ERR(xf->filp)) {
		int	ret = PTR_ERR(xf->filp);

		kmem_free(xf);
		return ERR_PTR(ret);
	}

	/*
	 * We want a large sparse file that we can pread, pwrite, and seek.
	 * xfile users are responsible for keeping the xfile hidden away from
	 * all other callers, so we skip timestamp updates and security checks.
	 */
	xf->filp->f_mode |= FMODE_PREAD | FMODE_PWRITE | FMODE_NOCMTIME |
			    FMODE_LSEEK;
	xf->filp->f_flags |= O_RDWR | O_LARGEFILE | O_NOATIME;
	xf->filp->f_inode->i_flags |= S_PRIVATE | S_NOCMTIME | S_NOATIME;

	trace_xfile_create(xf);
	return xf;
}

/* Close the file and release all resources. */
void
xfile_destroy(
	struct xfile	*xf)
{
	trace_xfile_destroy(xf, 0, 0);
	fput(xf->filp);
}

/* Read from the xfile. */
int
xfile_pread(
	struct xfile	*xf,
	void		*buf,
	size_t		count,
	loff_t		offset)
{
	ssize_t		ret;
	unsigned int	pflags;

	trace_xfile_pread(xf, offset, count);

	pflags = memalloc_nofs_save();
	ret = kernel_read(xf->filp, buf, count, &offset);
	memalloc_nofs_restore(pflags);

	/*
	 * Since we're treating this file as "memory", any IO error or short
	 * read should be treated as a failure to allocate memory.
	 */
	return ret == count ? 0 : -ENOMEM;
}

/* Write to the xfile. */
int
xfile_pwrite(
	struct xfile	*xf,
	void		*buf,
	size_t		count,
	loff_t		offset)
{
	ssize_t		ret;
	unsigned int	pflags;

	trace_xfile_pwrite(xf, offset, count);

	pflags = memalloc_nofs_save();
	ret = kernel_write(xf->filp, buf, count, &offset);
	memalloc_nofs_restore(pflags);

	/*
	 * Since we're treating this file as "memory", any IO error or short
	 * write should be treated as a failure to allocate memory.
	 */
	return ret == count ? 0 : -ENOMEM;
}

/* Discard pages backing a range of the file. */
void
xfile_discard(
	struct xfile	*xf,
	loff_t		start,
	loff_t		end)
{
	trace_xfile_discard(xf, start, end);
	shmem_truncate_range(file_inode(xf->filp), start, end);
}

/* Find the next batch of data at a given offset. */
loff_t
xfile_seek_data(
	struct xfile	*xf,
	loff_t		pos)
{
	loff_t		ret;

	ret = vfs_llseek(xf->filp, pos, SEEK_DATA);
	trace_xfile_seek_data(xf, pos, ret);
	return ret;
}

/* Query statx information for an xfile. */
int
xfile_statx(
	struct xfile	*xf,
	struct kstat	*statbuf)
{
	return vfs_getattr_nosec(&xf->filp->f_path, statbuf,
			STATX_SIZE | STATX_BLOCKS, AT_STATX_DONT_SYNC);
}
