// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
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
 * fput the file when finished.
 */
struct file *
xfile_create(
	const char	*description)
{
	struct file	*filp;

	filp = shmem_file_setup(description, 0, 0);
	if (IS_ERR_OR_NULL(filp))
		return filp;

	filp->f_mode |= FMODE_PREAD | FMODE_PWRITE | FMODE_NOCMTIME;
	filp->f_flags |= O_RDWR | O_LARGEFILE | O_NOATIME;
	return filp;
}

void
xfile_destroy(
	struct file	*filp)
{
	fput(filp);
}

/*
 * Perform a read or write IO to the file backing the array.  We can defer
 * the work to a workqueue if the caller so desires, either to reduce stack
 * usage or because the xfs is frozen and we want to avoid deadlocking on the
 * page fault that might be about to happen.
 */
int
xfile_io(
	struct file	*filp,
	unsigned int	cmd_flags,
	loff_t		*pos,
	void		*ptr,
	size_t		count)
{
	ssize_t		ret;
	unsigned int	pflags = memalloc_nofs_save();

	if ((cmd_flags & XFILE_IO_MASK) == XFILE_IO_READ)
		ret = kernel_read(filp, ptr, count, pos);
	else
		ret = kernel_write(filp, ptr, count, pos);
	memalloc_nofs_restore(pflags);

	/*
	 * Since we're treating this file as "memory", any IO error should be
	 * treated as a failure to find any memory.
	 */
	return ret == count ? 0 : -ENOMEM;
}

/* Discard pages backing a range of the file. */
void
xfile_discard(
	struct file	*filp,
	loff_t		start,
	loff_t		end)
{
	shmem_truncate_range(file_inode(filp), start, end);
}
