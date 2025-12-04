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
	refcount_set(&sb->s_pending_errors, 1);
}

static inline void fserror_unmount(struct super_block *sb)
{
	/*
	 * If we don't drop the pending error count to zero, then wait for it
	 * to drop below 1, which means that the pending errors cleared or
	 * that we saturated the system with 1 billion+ concurrent events.
	 */
	if (!refcount_dec_and_test(&sb->s_pending_errors))
		wait_var_event(&sb->s_pending_errors,
			       refcount_read(&sb->s_pending_errors) < 1);
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

	/* filesystem metadata */
	FSERR_METADATA,
};

struct fserror_event {
	struct work_struct work;
	struct super_block *sb;
	struct inode *inode;
	loff_t pos;
	u64 len;
	enum fserror_type type;

	/* negative error number */
	int error;
};

void fserror_report(struct super_block *sb, struct inode *inode,
		    enum fserror_type type, loff_t pos, u64 len, int error,
		    gfp_t gfp);

static inline void fserror_report_io(struct inode *inode,
				     enum fserror_type type, loff_t pos,
				     u64 len, int error, gfp_t gfp)
{
	fserror_report(inode->i_sb, inode, type, pos, len, error, gfp);
}

static inline void fserror_report_data_lost(struct inode *inode, loff_t pos,
					    u64 len, gfp_t gfp)
{
	fserror_report(inode->i_sb, inode, FSERR_DATA_LOST, pos, len, -EIO,
		       gfp);
}

static inline void fserror_report_file_metadata(struct inode *inode, int error,
						gfp_t gfp)
{
	fserror_report(inode->i_sb, inode, FSERR_METADATA, 0, 0, error, gfp);
}

static inline void fserror_report_metadata(struct super_block *sb, int error,
					   gfp_t gfp)
{
	fserror_report(sb, NULL, FSERR_METADATA, 0, 0, error, gfp);
}

static inline void fserror_report_shutdown(struct super_block *sb, gfp_t gfp)
{
	fserror_report(sb, NULL, FSERR_METADATA, 0, 0, -ESHUTDOWN, gfp);
}

#endif /* _LINUX_FSERROR_H__ */
