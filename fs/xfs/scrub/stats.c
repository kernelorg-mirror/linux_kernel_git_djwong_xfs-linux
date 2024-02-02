// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_sysfs.h"
#include "xfs_btree.h"
#include "xfs_super.h"
#include "xfs_timestats.h"
#include "scrub/scrub.h"
#include "scrub/stats.h"
#include "scrub/trace.h"

struct xchk_scrub_stats {
	/* all 32-bit counters here */

	/* checking stats */
	uint32_t		invocations;
	uint32_t		clean;
	uint32_t		corrupt;
	uint32_t		preen;
	uint32_t		xfail;
	uint32_t		xcorrupt;
	uint32_t		incomplete;
	uint32_t		warning;
	uint32_t		retries;

	/* repair stats */
	uint32_t		repair_invocations;
	uint32_t		repair_success;

	/* all 64-bit items here */

	/* runtimes */
	uint64_t		checktime_us;
	uint64_t		repairtime_us;

	/* non-counter state must go at the end for clearall */
	spinlock_t		css_lock;
};

struct xchk_timestats {
#ifdef CONFIG_XFS_TIME_STATS
	struct dentry		*parent;
	struct {
		struct time_stats	scrub;
		struct time_stats	repair;
	} scrub[XFS_SCRUB_TYPE_NR];
#endif
};

struct xchk_stats {
	struct dentry		*cs_debugfs;
#ifdef CONFIG_XFS_TIME_STATS
	struct xchk_timestats	*cs_timestats;
#endif
	struct xchk_scrub_stats	cs_stats[XFS_SCRUB_TYPE_NR];
};

static struct xchk_stats	global_stats;

static const char *name_map[XFS_SCRUB_TYPE_NR] = {
	[XFS_SCRUB_TYPE_SB]		= "sb",
	[XFS_SCRUB_TYPE_AGF]		= "agf",
	[XFS_SCRUB_TYPE_AGFL]		= "agfl",
	[XFS_SCRUB_TYPE_AGI]		= "agi",
	[XFS_SCRUB_TYPE_BNOBT]		= "bnobt",
	[XFS_SCRUB_TYPE_CNTBT]		= "cntbt",
	[XFS_SCRUB_TYPE_INOBT]		= "inobt",
	[XFS_SCRUB_TYPE_FINOBT]		= "finobt",
	[XFS_SCRUB_TYPE_RMAPBT]		= "rmapbt",
	[XFS_SCRUB_TYPE_REFCNTBT]	= "refcountbt",
	[XFS_SCRUB_TYPE_INODE]		= "inode",
	[XFS_SCRUB_TYPE_BMBTD]		= "bmapbtd",
	[XFS_SCRUB_TYPE_BMBTA]		= "bmapbta",
	[XFS_SCRUB_TYPE_BMBTC]		= "bmapbtc",
	[XFS_SCRUB_TYPE_DIR]		= "directory",
	[XFS_SCRUB_TYPE_XATTR]		= "xattr",
	[XFS_SCRUB_TYPE_SYMLINK]	= "symlink",
	[XFS_SCRUB_TYPE_PARENT]		= "parent",
	[XFS_SCRUB_TYPE_RTBITMAP]	= "rtbitmap",
	[XFS_SCRUB_TYPE_RTSUM]		= "rtsummary",
	[XFS_SCRUB_TYPE_UQUOTA]		= "usrquota",
	[XFS_SCRUB_TYPE_GQUOTA]		= "grpquota",
	[XFS_SCRUB_TYPE_PQUOTA]		= "prjquota",
	[XFS_SCRUB_TYPE_FSCOUNTERS]	= "fscounters",
	[XFS_SCRUB_TYPE_QUOTACHECK]	= "quotacheck",
	[XFS_SCRUB_TYPE_NLINKS]		= "nlinks",
	[XFS_SCRUB_TYPE_DIRTREE]	= "dirtree",
	[XFS_SCRUB_TYPE_METAPATH]	= "metapath",
	[XFS_SCRUB_TYPE_RGSUPER]	= "rgsuper",
	[XFS_SCRUB_TYPE_RGBITMAP]	= "rgbitmap",
	[XFS_SCRUB_TYPE_RTRMAPBT]	= "rtrmapbt",
	[XFS_SCRUB_TYPE_RTREFCBT]	= "rtrefcountbt",
};
#ifdef CONFIG_XFS_TIME_STATS
static inline void
xchk_timestats_init(
	struct xchk_stats	*cs,
	struct xfs_mount	*mp)
{
	struct xchk_timestats	*ts;
	unsigned int		i;

	/* Only individual mounts have timestats so far */
	if (!mp) {
		cs->cs_timestats = NULL;
		return;
	}

	/* timestats are optional */
	ts = kmalloc(sizeof(struct xchk_timestats), GFP_KERNEL);
	if (!ts) {
		cs->cs_timestats = NULL;
		return;
	}

	for (i = 0; i < XFS_SCRUB_TYPE_NR; i++) {
		time_stats_init(&ts->scrub[i].scrub);
		time_stats_init(&ts->scrub[i].repair);
	}

	ts->parent = mp->m_timestats.ts_debugfs;
	cs->cs_timestats = ts;
}

static inline void
xchk_timestats_teardown(
	struct xchk_stats	*cs)
{
	struct xchk_timestats	*ts = cs->cs_timestats;
	unsigned int		i;

	if (!ts)
		return;

	for (i = 0; i < XFS_SCRUB_TYPE_NR; i++) {
		time_stats_exit(&ts->scrub[i].scrub);
		time_stats_exit(&ts->scrub[i].repair);
	}
	kfree(ts);
	cs->cs_timestats = NULL;
}

static inline void
xchk_timestats_register(
	struct xchk_stats	*cs)
{
	char			name[32];
	struct xchk_timestats	*ts = cs->cs_timestats;
	unsigned int		i;

	if (!ts)
		return;

	for (i = 0; i < XFS_SCRUB_TYPE_NR; i++) {
		if (!name_map[i])
			continue;

		snprintf(name, 32, "scrub::%s", name_map[i]);
		debugfs_create_file(name, 0444, ts->parent,
				&ts->scrub[i].scrub, &xfs_timestats_fops);

		snprintf(name, 32, "repair::%s", name_map[i]);
		debugfs_create_file(name, 0444, ts->parent,
				&ts->scrub[i].repair, &xfs_timestats_fops);
	}
}

STATIC void
xchk_timestats_merge_one(
	struct xchk_stats		*cs,
	const struct xfs_scrub_metadata	*sm,
	const struct xchk_stats_run	*run)
{
	struct xchk_timestats		*ts = cs->cs_timestats;

	if (sm->sm_type >= XFS_SCRUB_TYPE_NR) {
		ASSERT(sm->sm_type < XFS_SCRUB_TYPE_NR);
		return;
	}
	if (!ts)
		return;

	xfs_timestats_interval(&ts->scrub[sm->sm_type].scrub,
			run->scrub_start, run->scrub_stop);
	xfs_timestats_interval(&ts->scrub[sm->sm_type].repair,
			run->repair_start, run->repair_stop);
}

#else
# define xchk_timestats_init(cs, mp)	((void)0)
# define xchk_timestats_teardown(cs)	((void)0)
# define xchk_timestats_register(cs)	((void)0)
# define xchk_timestats_merge_one(...)	((void)0)
#endif

/* Format the scrub stats into a text buffer, similar to pcp style. */
STATIC ssize_t
xchk_stats_format(
	struct xchk_stats	*cs,
	char			*buf,
	size_t			remaining)
{
	struct xchk_scrub_stats	*css = &cs->cs_stats[0];
	unsigned int		i;
	ssize_t			copied = 0;
	int			ret = 0;

	for (i = 0; i < XFS_SCRUB_TYPE_NR; i++, css++) {
		if (!name_map[i])
			continue;

		ret = scnprintf(buf, remaining,
 "%s %u %u %u %u %u %u %u %u %u %llu %u %u %llu\n",
				name_map[i],
				(unsigned int)css->invocations,
				(unsigned int)css->clean,
				(unsigned int)css->corrupt,
				(unsigned int)css->preen,
				(unsigned int)css->xfail,
				(unsigned int)css->xcorrupt,
				(unsigned int)css->incomplete,
				(unsigned int)css->warning,
				(unsigned int)css->retries,
				(unsigned long long)css->checktime_us,
				(unsigned int)css->repair_invocations,
				(unsigned int)css->repair_success,
				(unsigned long long)css->repairtime_us);
		if (ret <= 0)
			break;

		remaining -= ret;
		copied += ret;
		buf +=  ret;
	}

	return copied > 0 ? copied : ret;
}

/* Estimate the worst case buffer size required to hold the whole report. */
STATIC size_t
xchk_stats_estimate_bufsize(
	struct xchk_stats	*cs)
{
	struct xchk_scrub_stats	*css = &cs->cs_stats[0];
	unsigned int		i;
	size_t			field_width;
	size_t			ret = 0;

	/* 4294967296 plus one space for each u32 field */
	field_width = 11 * (offsetof(struct xchk_scrub_stats, checktime_us) /
			    sizeof(uint32_t));

	/* 18446744073709551615 plus one space for each u64 field */
	field_width += 21 * ((offsetof(struct xchk_scrub_stats, css_lock) -
			      offsetof(struct xchk_scrub_stats, checktime_us)) /
			     sizeof(uint64_t));

	for (i = 0; i < XFS_SCRUB_TYPE_NR; i++, css++) {
		if (!name_map[i])
			continue;

		/* name plus one space */
		ret += 1 + strlen(name_map[i]);

		/* all fields, plus newline */
		ret += field_width + 1;
	}

	return ret;
}

/* Clear all counters. */
STATIC void
xchk_stats_clearall(
	struct xchk_stats	*cs)
{
	struct xchk_scrub_stats	*css = &cs->cs_stats[0];
	unsigned int		i;

	for (i = 0; i < XFS_SCRUB_TYPE_NR; i++, css++) {
		spin_lock(&css->css_lock);
		memset(css, 0, offsetof(struct xchk_scrub_stats, css_lock));
		spin_unlock(&css->css_lock);
	}
}

#define XFS_SCRUB_OFLAG_UNCLEAN	(XFS_SCRUB_OFLAG_CORRUPT | \
				 XFS_SCRUB_OFLAG_PREEN | \
				 XFS_SCRUB_OFLAG_XFAIL | \
				 XFS_SCRUB_OFLAG_XCORRUPT | \
				 XFS_SCRUB_OFLAG_INCOMPLETE | \
				 XFS_SCRUB_OFLAG_WARNING)

STATIC void
xchk_stats_merge_one(
	struct xchk_stats		*cs,
	const struct xfs_scrub_metadata	*sm,
	const struct xchk_stats_run	*run)
{
	struct xchk_scrub_stats		*css;
	u64				delta;

	if (sm->sm_type >= XFS_SCRUB_TYPE_NR) {
		ASSERT(sm->sm_type < XFS_SCRUB_TYPE_NR);
		return;
	}

	css = &cs->cs_stats[sm->sm_type];
	spin_lock(&css->css_lock);
	css->invocations++;
	if (!(sm->sm_flags & XFS_SCRUB_OFLAG_UNCLEAN))
		css->clean++;
	if (sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT)
		css->corrupt++;
	if (sm->sm_flags & XFS_SCRUB_OFLAG_PREEN)
		css->preen++;
	if (sm->sm_flags & XFS_SCRUB_OFLAG_XFAIL)
		css->xfail++;
	if (sm->sm_flags & XFS_SCRUB_OFLAG_XCORRUPT)
		css->xcorrupt++;
	if (sm->sm_flags & XFS_SCRUB_OFLAG_INCOMPLETE)
		css->incomplete++;
	if (sm->sm_flags & XFS_SCRUB_OFLAG_WARNING)
		css->warning++;
	css->retries += run->retries;
	delta = max(1, run->scrub_stop - run->scrub_start);
	css->checktime_us += howmany_64(delta, NSEC_PER_USEC);

	if (run->repair_attempted)
		css->repair_invocations++;
	if (run->repair_succeeded)
		css->repair_success++;
	delta = max(1, run->repair_stop - run->repair_start);
	css->repairtime_us += howmany_64(delta, NSEC_PER_USEC);
	spin_unlock(&css->css_lock);
}

/* Merge these scrub-run stats into the global and mount stat data. */
void
xchk_stats_merge(
	struct xfs_mount		*mp,
	const struct xfs_scrub_metadata	*sm,
	const struct xchk_stats_run	*run)
{
	xchk_stats_merge_one(&global_stats, sm, run);
	xchk_stats_merge_one(mp->m_scrub_stats, sm, run);
	xchk_timestats_merge_one(mp->m_scrub_stats, sm, run);
}

/* debugfs boilerplate */

static ssize_t
xchk_scrub_stats_read(
	struct file		*file,
	char __user		*ubuf,
	size_t			count,
	loff_t			*ppos)
{
	struct xchk_stats	*cs = file->private_data;
	char			*buf;
	size_t			bufsize;
	ssize_t			avail, ret;

	/*
	 * This generates stringly snapshot of all the scrub counters, so we
	 * do not want userspace to receive garbled text from multiple calls.
	 * If the file position is greater than 0, return a short read.
	 */
	if (*ppos > 0)
		return 0;

	bufsize = xchk_stats_estimate_bufsize(cs);

	buf = kvmalloc(bufsize, XCHK_GFP_FLAGS);
	if (!buf)
		return -ENOMEM;

	avail = xchk_stats_format(cs, buf, bufsize);
	if (avail < 0) {
		ret = avail;
		goto out;
	}

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, avail);
out:
	kvfree(buf);
	return ret;
}

static const struct file_operations scrub_stats_fops = {
	.open			= simple_open,
	.read			= xchk_scrub_stats_read,
};

static ssize_t
xchk_clear_scrub_stats_write(
	struct file		*file,
	const char __user	*ubuf,
	size_t			count,
	loff_t			*ppos)
{
	struct xchk_stats	*cs = file->private_data;
	unsigned int		val;
	int			ret;

	ret = kstrtouint_from_user(ubuf, count, 0, &val);
	if (ret)
		return ret;

	if (val != 1)
		return -EINVAL;

	xchk_stats_clearall(cs);
	return count;
}

static const struct file_operations clear_scrub_stats_fops = {
	.open			= simple_open,
	.write			= xchk_clear_scrub_stats_write,
};

/* Initialize the stats object. */
STATIC int
xchk_stats_init(
	struct xchk_stats	*cs,
	struct xfs_mount	*mp)
{
	struct xchk_scrub_stats	*css = &cs->cs_stats[0];
	unsigned int		i;

	for (i = 0; i < XFS_SCRUB_TYPE_NR; i++, css++)
		spin_lock_init(&css->css_lock);

	xchk_timestats_init(cs, mp);
	return 0;
}

/* Connect the stats object to debugfs. */
void
xchk_stats_register(
	struct xchk_stats	*cs,
	struct dentry		*parent)
{
	if (!parent)
		return;

	cs->cs_debugfs = xfs_debugfs_mkdir("scrub", parent);
	if (!cs->cs_debugfs)
		return;

	debugfs_create_file("stats", 0444, cs->cs_debugfs, cs,
			&scrub_stats_fops);
	debugfs_create_file("clear_stats", 0200, cs->cs_debugfs, cs,
			&clear_scrub_stats_fops);

	xchk_timestats_register(cs);
}

/* Free all resources related to the stats object. */
STATIC int
xchk_stats_teardown(
	struct xchk_stats	*cs)
{
	xchk_timestats_teardown(cs);
	return 0;
}

/* Disconnect the stats object from debugfs. */
void
xchk_stats_unregister(
	struct xchk_stats	*cs)
{
	debugfs_remove(cs->cs_debugfs);
}

/* Initialize global stats and register them */
int __init
xchk_global_stats_setup(
	struct dentry		*parent)
{
	int			error;

	error = xchk_stats_init(&global_stats, NULL);
	if (error)
		return error;

	xchk_stats_register(&global_stats, parent);
	return 0;
}

/* Unregister global stats and tear them down */
void
xchk_global_stats_teardown(void)
{
	xchk_stats_unregister(&global_stats);
	xchk_stats_teardown(&global_stats);
}

/* Allocate per-mount stats */
int
xchk_mount_stats_alloc(
	struct xfs_mount	*mp)
{
	struct xchk_stats	*cs;
	int			error;

	cs = kvzalloc(sizeof(struct xchk_stats), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;

	error = xchk_stats_init(cs, mp);
	if (error)
		goto out_free;

	mp->m_scrub_stats = cs;
	return 0;
out_free:
	kvfree(cs);
	return error;
}

/* Free per-mount stats */
void
xchk_mount_stats_free(
	struct xfs_mount	*mp)
{
	xchk_stats_teardown(mp->m_scrub_stats);
	kvfree(mp->m_scrub_stats);
	mp->m_scrub_stats = NULL;
}
