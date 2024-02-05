// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_timestats.h"

/* Format a timestats report into a buffer. */
static ssize_t
xfs_timestats_read(
	struct file		*file,
	char __user		*ubuf,
	size_t			count,
	loff_t			*ppos)
{
	struct seq_buf		s;
	struct time_stats	*ts = file->private_data;
	char			*buf;
	ssize_t			ret;

	/*
	 * This generates a stringly snapshot of a timestats report, so we
	 * do not want userspace to receive garbled text from multiple calls.
	 * If the file position is greater than 0, return a short read.
	 */
	if (*ppos > 0)
		return 0;

	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	seq_buf_init(&s, buf, PAGE_SIZE);
	time_stats_to_seq_buf(&s, ts, "mount", TIME_STATS_PRINT_NO_ZEROES);
	ret = simple_read_from_buffer(ubuf, count, ppos, buf, seq_buf_used(&s));
	kfree(buf);
	return ret;
}

const struct file_operations xfs_timestats_fops = {
	.open			= simple_open,
	.read			= xfs_timestats_read,
};

/* Format a timestats report into a buffer as json. */
static ssize_t
xfs_timestats_read_json(
	struct file		*file,
	char __user		*ubuf,
	size_t			count,
	loff_t			*ppos)
{
	struct seq_buf		s;
	struct time_stats	*ts = file->private_data;
	char			*buf;
	ssize_t			ret;

	/*
	 * This generates a stringly snapshot of a timestats report, so we
	 * do not want userspace to receive garbled text from multiple calls.
	 * If the file position is greater than 0, return a short read.
	 */
	if (*ppos > 0)
		return 0;

	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	seq_buf_init(&s, buf, PAGE_SIZE);
	time_stats_to_json(&s, ts, "mount", TIME_STATS_PRINT_NO_ZEROES);
	ret = simple_read_from_buffer(ubuf, count, ppos, buf, seq_buf_used(&s));
	kfree(buf);
	return ret;
}

const struct file_operations xfs_timestats_json_fops = {
	.open			= simple_open,
	.read			= xfs_timestats_read_json,
};

/* Set up timestats collection. */
void
xfs_timestats_init(
	struct xfs_mount	*mp)
{
	struct xfs_timestats	*ts = &mp->m_timestats;

	time_stats_init(&ts->ts_log_reserve);
	time_stats_init(&ts->ts_log_regrant);
	time_stats_init(&ts->ts_ilock);
	time_stats_init(&ts->ts_buflock);
	time_stats_init(&ts->ts_dqlock);
}

/* Free all resources used by timestats collection. */
void
xfs_timestats_destroy(
	struct xfs_mount	*mp)
{
	struct xfs_timestats	*ts = &mp->m_timestats;

	time_stats_exit(&ts->ts_log_reserve);
	time_stats_exit(&ts->ts_log_regrant);
	time_stats_exit(&ts->ts_ilock);
	time_stats_exit(&ts->ts_buflock);
	time_stats_exit(&ts->ts_dqlock);
}

/* Export timestats via debugfs */
#define X(p, ts, name) \
	do { \
		debugfs_create_file("blocked::" #name ".txt", 0444, (p), \
				&(ts)->ts_##name, &xfs_timestats_fops); \
		debugfs_create_file("blocked::" #name ".json", 0444, (p), \
				&(ts)->ts_##name, &xfs_timestats_json_fops); \
	} while (0)
void
xfs_timestats_export(
	struct xfs_mount	*mp)
{
	struct dentry		*parent;
	struct xfs_timestats	*ts = &mp->m_timestats;

	if (!mp->m_debugfs)
		return;

	parent = xfs_debugfs_mkdir("time_stats", mp->m_debugfs);
	if (!parent)
		return;
	ts->ts_debugfs = parent;

	X(parent, ts, log_reserve);
	X(parent, ts, log_regrant);
	X(parent, ts, ilock);
	X(parent, ts, buflock);
	X(parent, ts, dqlock);
}
#undef X

/* Delete debugfs entries for timestats */
void
xfs_timestats_unexport(
	struct xfs_mount	*mp)
{
	struct xfs_timestats	*ts = &mp->m_timestats;

	debugfs_remove(ts->ts_debugfs);
}
