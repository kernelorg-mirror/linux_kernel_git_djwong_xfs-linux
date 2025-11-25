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
		struct fs_error_report report = {
			/* send positive error number to userspace */
			.error = -event->error,
			.inode = event->inode,
			.sb = event->sb,
		};

		if (sb->s_op->report_error)
			sb->s_op->report_error(event);

		fsnotify(FS_ERROR, &report, FSNOTIFY_EVENT_ERROR, NULL, NULL,
			 NULL, 0);
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

	/*
	 * Use schedule_work here even if we're already in process context so
	 * that fsnotify and super_operations::report_error implementations are
	 * guaranteed to run in process context without any locks held.  Since
	 * errors are supposed to be rare, the overhead shouldn't kill us any
	 * more than the failing device will.
	 */
	schedule_work(&event->work);
}

/**
 * fserror_report - report a filesystem error of some kind
 *
 * Report details of a filesystem error to the super_operations::report_error
 * callback if present; and to fsnotify for distribution to userspace.  @sb,
 * @gfp, @type, and @error must all be specified.  For file I/O errors, the
 * @inode, @pos, and @len fields must also be specified.  For file metadata
 * errors, @inode must be specified.  If @inode is not NULL, then @inode->i_sb
 * must point to @sb.
 *
 * Reporting work is deferred to a workqueue to ensure that ->report_error is
 * called from process context without any locks held.  An active reference to
 * the inode is maintained until event handling is complete, and unmount will
 * wait for queued events to drain.
 *
 * @sb:		superblock of the filesystem
 * @inode:	inode within that filesystem, if applicable
 * @type:	type of error encountered
 * @pos:	start of inode range affected, if applicable
 * @len:	length of inode range affected, if applicable
 * @error:	error number encountered, must be negative
 * @gfp:	memory allocation flags for conveying the event to a worker,
 *		since this function can be called from atomic contexts
 */
void fserror_report(struct super_block *sb, struct inode *inode,
		    enum fserror_type type, loff_t pos, u64 len, int error,
		    gfp_t gfp)
{
	struct fserror_event *event;

	/* sb and inode must be from the same filesystem */
	WARN_ON_ONCE(inode && inode->i_sb != sb);

	/* error number must be negative */
	WARN_ON_ONCE(error >= 0);

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
	event->error = error;

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
