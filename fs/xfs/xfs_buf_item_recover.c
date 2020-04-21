// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_mount.h"
#include "xfs_trans.h"
#include "xfs_buf_item.h"
#include "xfs_trans_priv.h"
#include "xfs_trace.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_log_recover.h"

STATIC enum xlog_recover_reorder
xlog_buf_reorder_fn(
	struct xlog_recover_item	*item)
{
	struct xfs_buf_log_format	*buf_f = item->ri_buf[0].i_addr;

	if (buf_f->blf_flags & XFS_BLF_CANCEL)
		return XLOG_REORDER_CANCEL_LIST;
	if (buf_f->blf_flags & XFS_BLF_INODE_BUF)
		return XLOG_REORDER_INODE_BUFFER_LIST;
	return XLOG_REORDER_BUFFER_LIST;
}

STATIC void
xlog_recover_buffer_ra_pass2(
	struct xlog                     *log,
	struct xlog_recover_item        *item)
{
	struct xfs_buf_log_format	*buf_f = item->ri_buf[0].i_addr;

	xlog_buf_readahead(log, buf_f->blf_blkno, buf_f->blf_len, NULL);
}

const struct xlog_recover_item_type xlog_buf_item_type = {
	.reorder_fn		= xlog_buf_reorder_fn,
	.ra_pass2_fn		= xlog_recover_buffer_ra_pass2,
};
