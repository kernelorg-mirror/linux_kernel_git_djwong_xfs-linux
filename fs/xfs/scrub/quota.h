// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_QUOTA_H__
#define __XFS_SCRUB_QUOTA_H__

typedef int (*xchk_dqiterate_fn)(struct xfs_dquot *dq,
		xfs_dqtype_t type, void *priv);
int xchk_dqiterate(struct xfs_mount *mp, xfs_dqtype_t type,
		xchk_dqiterate_fn iter_fn, void *priv);

#endif /* __XFS_SCRUB_QUOTA_H__ */
