// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <linux/fs.h>
#include <linux/fsnotify.h>
#include <linux/mempool.h>
#include <linux/fserror.h>

#define FSERROR_DEFAULT_EVENT_POOL_SIZE		(32)

static struct mempool fserror_events_pool;

static void fserror_worker(struct work_struct *work)
{
	struct fserror_event *event =
			container_of(work, struct fserror_event, work);
	struct super_block *sb = event->sb;

	if (sb->s_flags & SB_ACTIVE) {
		if (sb->s_op->report_error)
			sb->s_op->report_error(event);
		fsnotify_sb_error(sb, event->inode, event->error);
	}

	iput(event->inode);
	if (atomic_dec_and_test(&sb->s_pending_errors))
		wake_up_var(&sb->s_pending_errors);

	mempool_free(event, &fserror_events_pool);
}

static inline struct fserror_event *fserror_alloc_event(void)
{
	/* mempool_alloc doesn't support GFP_ZERO */
	struct fserror_event *event =
			mempool_alloc(&fserror_events_pool, GFP_ATOMIC);
	if (event)
		memset(event, 0, sizeof(*event));
	return event;
}

static inline void fserror_submit_event(struct super_block *sb,
					struct fserror_event *event)
{
	INIT_WORK(&event->work, fserror_worker);
	atomic_inc(&sb->s_pending_errors);
	schedule_work(&event->work);
}

/**
 * Report a file I/O error.
 *
 * Event dispatch work is deferred to a workqueue to avoid problems with
 * already-held locks.  An active reference to the inode is maintained until
 * event handling is complete, and unmount will wait for queued events.
 *
 * @inode Inode within filesystem
 * @type Type of error
 * @pos Start of file range affected
 * @len Length of file range affected
 * @error Error encountered.
 */
void fserror_report_fileio(struct inode *inode, enum fserror_type type,
			   loff_t pos, u64 len, int error)
{
	struct super_block *sb = inode->i_sb;
	struct fserror_event *event;
	struct inode *ninode;

	/*
	 * Ignore events if nobody asked for it or the super is being torn down
	 * around us.
	 */
	if (!(sb->s_flags & SB_ACTIVE))
		return;

	event = fserror_alloc_event();
	if (!event)
		goto lost;

	/* Can't iput from non-sleeping context, this must come last */
	ninode = igrab(inode);
	if (!ninode)
		goto lost_event;

	event->inode = ninode;
	event->sb = sb;
	event->type = type;
	event->pos = pos;
	event->len = len;
	event->error = error;

	fserror_submit_event(sb, event);
	return;

lost_event:
	mempool_free(event, &fserror_events_pool);
lost:
	printk(KERN_ERR
 "%s: lost file I/O error report for ino %lu type %u pos 0x%llx len 0x%llx error %d",
	       sb->s_id, inode->i_ino, type, pos, len, error);
}
EXPORT_SYMBOL_GPL(fserror_report_fileio);

/**
 * Report a filesystem metadata / internal error.
 *
 * Event dispatch work is deferred to a workqueue to avoid problems with
 * already-held locks.  Unmount will wait for queued events.
 *
 * @sb The filesystem in question
 * @error Error encountered.
 */
void fserror_report_metadata(struct super_block *sb, int error)
{
	struct fserror_event *event;

	/*
	 * Ignore events if nobody asked for it or the super is being torn down
	 * around us.
	 */
	if (!(sb->s_flags & SB_ACTIVE))
		return;

	event = fserror_alloc_event();
	if (!event) {
		printk(KERN_ERR "%s: lost filesystem error report error=%d",
		       sb->s_id, error);
		return;
	}

	event->sb = sb;
	event->type = FSERR_METADATA;
	event->error = error;

	fserror_submit_event(sb, event);
}
EXPORT_SYMBOL_GPL(fserror_report_metadata);

static int __init fserror_init(void)
{
	return mempool_init_kmalloc_pool(&fserror_events_pool,
					 FSERROR_DEFAULT_EVENT_POOL_SIZE,
					 sizeof(struct fserror_event));
}
fs_initcall(fserror_init);
