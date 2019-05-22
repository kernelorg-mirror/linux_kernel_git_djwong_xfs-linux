// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SCRUB_XFILE_H__
#define __XFS_SCRUB_XFILE_H__

struct file *xfile_create(const char *description);
void xfile_destroy(struct file *filp);

/* read or write? */
#define XFILE_IO_READ		(0)
#define XFILE_IO_WRITE		(1)
#define XFILE_IO_MASK		(1 << 0)
int xfile_io(struct file *filp, unsigned int cmd_flags, loff_t *pos,
		void *ptr, size_t count);

void xfile_discard(struct file *filp, loff_t start, loff_t end);

#endif /* __XFS_SCRUB_XFILE_H__ */
