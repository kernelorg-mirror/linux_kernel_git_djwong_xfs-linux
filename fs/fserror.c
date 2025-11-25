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

static inline struct fserror_event *fserror_alloc_event(gfp_t gfp_flags)
{
	/* mempool_alloc doesn't support GFP_ZERO */
	struct fserror_event *event =
			mempool_alloc(&fserror_events_pool, gfp_flags);
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
 * Report a filesystem error of some kind.
 *
 * Event dispatch work is deferred to a workqueue to avoid problems with
 * already-held locks.  An active reference to the inode is maintained until
 * event handling is complete, and unmount will wait for queued events.
 *
 * @sb Superblock of the filesystem.
 * @inode Inode within filesystem, if applicable.
 * @gfp Memory allocation flags for conveying the event to a worker, since
 *      this function can be called from atomic contexts.
 * @type Type of error
 * @pos Start of inode range affected
 * @len Length of inode range affected
 * @error Error encountered.
 */
void fserror_report(struct super_block *sb, struct inode *inode, gfp_t gfp,
		    enum fserror_type type, loff_t pos, u64 len, int error)
{
	struct fserror_event *event;

	/* sb and inode must be from the same filesystem */
	WARN_ON_ONCE(inode && inode->i_sb != sb);

	/*
	 * Ignore events if nobody asked for it or the super is being torn down
	 * around us.
	 */
	if (!(sb->s_flags & SB_ACTIVE))
		return;

	event = fserror_alloc_event(gfp);
	if (!event)
		goto lost;

	event->sb = sb;
	event->type = type;
	event->pos = pos;
	event->len = len;
	event->error = abs(error);

	/*
	 * Can't iput from non-sleeping context, so grabbing another reference
	 * to the inode must be the last thing before submitting the event.
	 */
	if (inode) {
		event->inode = igrab(inode);
		if (!event->inode) {
			mempool_free(event, &fserror_events_pool);
			goto lost;
		}
	}

	fserror_submit_event(sb, event);
	return;

lost:
	if (inode)
		pr_err(
 "%s: lost file I/O error report for ino %lu type %u pos 0x%llx len 0x%llx error %d",
		       sb->s_id, inode->i_ino, type, pos, len, error);
	else
		pr_err(
 "%s: lost filesystem error report for type %u error %d",
		       sb->s_id, type, error);
}
EXPORT_SYMBOL_GPL(fserror_report);

static int __init fserror_init(void)
{
	return mempool_init_kmalloc_pool(&fserror_events_pool,
					 FSERROR_DEFAULT_EVENT_POOL_SIZE,
					 sizeof(struct fserror_event));
}
fs_initcall(fserror_init);
