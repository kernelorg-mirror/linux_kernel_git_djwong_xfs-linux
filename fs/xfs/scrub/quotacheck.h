/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_QUOTACHECK_H__
#define __XFS_SCRUB_QUOTACHECK_H__

/* Quota counters for live quotacheck. */
struct xqcheck_dquot {
	/* block usage count */
	int64_t			bcount;

	/* inode usage count */
	int64_t			icount;

	/* realtime block usage count */
	int64_t			rtbcount;

	/* Record state */
	unsigned int		flags;
};

/* Already checked this dquot */
#define XQCHECK_DQUOT_COMPARE_SCANNED	(1U << 0)

/* Live quotacheck control structure. */
struct xqcheck {
	struct xfs_scrub	*sc;

	/* Shadow dquot counter data. */
	struct xfarray		*ucounts;
	struct xfarray		*gcounts;
	struct xfarray		*pcounts;

	/* Lock protecting quotacheck count observations */
	struct mutex		lock;

	struct xchk_iscan	iscan;

	/* Hooks into the quota code. */
	struct notifier_block	mod_dqtrx_hook;
	struct notifier_block	apply_dqtrx_hook;

	/* Shadow quota delta tracking structure. */
	struct rhashtable	shadow_dquot_acct;
};

/* Return the incore counter array for a given quota type. */
static inline struct xfarray *
xqcheck_counters_for(
	struct xqcheck		*xqc,
	xfs_dqtype_t		dqtype)
{
	switch (dqtype) {
	case XFS_DQTYPE_USER:
		return xqc->ucounts;
	case XFS_DQTYPE_GROUP:
		return xqc->gcounts;
	case XFS_DQTYPE_PROJ:
		return xqc->pcounts;
	}

	ASSERT(0);
	return NULL;
}

#endif /* __XFS_SCRUB_QUOTACHECK_H__ */
