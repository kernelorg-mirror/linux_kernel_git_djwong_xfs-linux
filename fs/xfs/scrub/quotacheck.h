/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SCRUB_QUOTACHECK_H__
#define __XFS_SCRUB_QUOTACHECK_H__

/* Quota counters for live quotacheck. */
struct xqcheck_dquot {
	int64_t			bcount;
	int64_t			icount;
	int64_t			rtbcount;
};

/* Live quotacheck control structure. */
struct xqcheck {
	struct xfs_scrub	*sc;

	/* Shadow dquot counter data. */
	struct xfbma		*ucounts;
	struct xfbma		*gcounts;
	struct xfbma		*pcounts;
};

/* Return the incore counter array for a given quota type. */
static inline struct xfbma *
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

int xqcheck_get_shadow_dquot(struct xfbma *counts, xfs_dqid_t id,
		struct xqcheck_dquot *xcdq);

#endif /* __XFS_SCRUB_QUOTACHECK_H__ */
