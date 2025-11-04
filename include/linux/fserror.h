/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef _LINUX_FSERROR_H__
#define _LINUX_FSERROR_H__

static inline void fserror_mount(struct super_block *sb)
{
	/*
	 * The pending error counter is biased by 1 so that we don't wake_var
	 * until we're actually trying to unmount.
	 */
	atomic_set(&sb->s_pending_errors, 1);
}

static inline void fserror_unmount(struct super_block *sb)
{
	if (atomic_dec_and_test(&sb->s_pending_errors))
		wait_var_event(&sb->s_pending_errors,
			       !atomic_read(&sb->s_pending_errors));
}

enum fserror_type {
	/* pagecache I/O failed */
	FSERR_BUFFERED_READ,
	FSERR_BUFFERED_WRITE,

	/* direct I/O failed */
	FSERR_DIRECTIO_READ,
	FSERR_DIRECTIO_WRITE,

	/* out of band media error reported */
	FSERR_DATA_LOST,
};

struct fserror_event {
	struct work_struct work;
	struct super_block *sb;
	struct inode *inode;
	loff_t pos;
	u64 len;
	enum fserror_type type;
	int error;
};

void fserror_report_fileio(struct inode *inode, enum fserror_type type,
			   loff_t pos, u64 len, int error);

static inline void fserror_report_datalost(struct inode *inode,
			   loff_t pos, u64 len)
{
	fserror_report_fileio(inode, FSERR_DATA_LOST, pos, len, -EIO);
}

#endif /* _LINUX_FSERROR_H__ */
