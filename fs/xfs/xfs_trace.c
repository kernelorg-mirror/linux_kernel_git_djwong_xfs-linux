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
#include <linux/iomap.h>
#include "xfs_iomap.h"
#include "scrub/xfile.h"
#include "scrub/xfbtree.h"
#include "xfs_btree_mem.h"
#include "xfs_bmap.h"
#include "xfs_swapext.h"
#include "xfs_xchgrange.h"
#include "xfs_parent.h"
#include "xfs_imeta.h"
#include "xfs_rtgroup.h"
#include "xfs_rmap.h"
#include "xfs_refcount.h"
#include "xfs_fsrefs.h"

static inline void
xfs_rmapbt_crack_agno_opdev(
	struct xfs_btree_cur	*cur,
	xfs_agnumber_t		*agno,
	dev_t			*opdev)
{
	if (cur->bc_flags & XFS_BTREE_IN_XFILE) {
		*agno = 0;
		*opdev = xfbtree_target(cur->bc_mem.xfbtree)->bt_dev;
	} else if (cur->bc_flags & XFS_BTREE_ROOT_IN_INODE) {
		*agno = cur->bc_ino.rtg->rtg_rgno;
		*opdev = cur->bc_mp->m_rtdev_targp->bt_dev;
	} else {
		*agno = cur->bc_ag.pag->pag_agno;
		*opdev = cur->bc_mp->m_super->s_dev;
	}
}

static inline void
xfs_refcountbt_crack_agno_opdev(
	struct xfs_btree_cur	*cur,
	xfs_agnumber_t		*agno,
	dev_t			*opdev)
{
	return xfs_rmapbt_crack_agno_opdev(cur, agno, opdev);
}

/*
 * We include this last to have the helpers above available for the trace
 * event implementations.
 */
#define CREATE_TRACE_POINTS
#include "xfs_trace.h"
