// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2009, Christoph Hellwig
 * All Rights Reserved.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_bit.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_da_format.h"
#include "xfs_inode.h"
#include "xfs_btree.h"
#include "xfs_da_btree.h"
#include "xfs_alloc.h"
#include "xfs_bmap.h"
#include "xfs_attr.h"
#include "xfs_trans.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_buf_item.h"
#include "xfs_quota.h"
#include "xfs_dquot_item.h"
#include "xfs_dquot.h"
#include "xfs_log_recover.h"
#include "xfs_filestream.h"
#include "xfs_fsmap.h"
#include "xfs_btree_staging.h"
#include "xfs_icache.h"
#include "xfs_ag.h"
#include "xfs_ag_resv.h"
#include "xfs_error.h"
#include "xfs_bmap.h"
#include "xfs_swapext.h"
#include "xfs_xchgrange.h"
#include "xfs_rtalloc.h"
#include "xfs_rmap.h"

static inline void
xfs_btree_crack_agno_opdev(
	struct xfs_btree_cur	*cur,
	xfs_agnumber_t		*agno,
	dev_t			*opdev)
{
	if (cur->bc_flags & XFS_BTREE_ROOT_IN_INODE) {
		*agno = 0;
		*opdev = cur->bc_mp->m_rtdev_targp->bt_dev;
	} else {
		*agno = cur->bc_ag.pag->pag_agno;
		*opdev = cur->bc_mp->m_super->s_dev;
	}
}

/*
 * We include this last to have the helpers above available for the trace
 * event implementations.
 */
#define CREATE_TRACE_POINTS
#include "xfs_trace.h"
