// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_bit.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_inode.h"
#include "xfs_quota.h"
#include "xfs_qm.h"
#include "xfs_bmap.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/quota.h"

/*
 * Iterate every dquot of a particular type.  The caller must ensure that the
 * particular quota type is active.  iter_fn can return negative error codes,
 * or -ECANCELED to indicate that it wants to stop iterating.
 */
int
xchk_dqiterate(
	struct xfs_mount	*mp,
	xfs_dqtype_t		type,
	xchk_dqiterate_fn	iter_fn,
	void			*priv)
{
	struct xfs_dquot	*dq;
	xfs_dqid_t		id = 0;
	int			error;

	do {
		error = xfs_qm_dqget_next(mp, id, type, &dq);
		if (error == -ENOENT)
			return 0;
		if (error)
			return error;

		error = iter_fn(dq, type, priv);
		id = dq->q_id + 1;
		xfs_qm_dqput(dq);
	} while (error == 0 && id != 0);

	return error;
}
