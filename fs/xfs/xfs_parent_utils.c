// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022-2024 Oracle.
 * All rights reserved.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_bmap_btree.h"
#include "xfs_inode.h"
#include "xfs_error.h"
#include "xfs_trace.h"
#include "xfs_trans.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_attr.h"
#include "xfs_ioctl.h"
#include "xfs_parent.h"
#include "xfs_da_btree.h"
#include "xfs_parent_utils.h"
#include "xfs_health.h"

struct xfs_getparent_ctx {
	struct xfs_attr_list_context	context;
	struct xfs_parent_irec		pptr_irec;
	struct xfs_getparents		*ppi;
};

static inline unsigned int
xfs_getparents_rec_sizeof(
	const struct xfs_parent_irec	*irec,
	unsigned int			namelen)
{
	return round_up(sizeof(struct xfs_getparents_rec) + namelen + 1,
			sizeof(uint32_t));
}

static void
xfs_getparent_listent(
	struct xfs_attr_list_context	*context,
	int				flags,
	unsigned char			*name,
	int				namelen,
	void				*value,
	int				valuelen)
{
	struct xfs_getparent_ctx	*gp;
	struct xfs_getparents		*ppi;
	struct xfs_getparents_rec	*pptr;
	int				arraytop;
	int				ret;

	gp = container_of(context, struct xfs_getparent_ctx, context);
	ppi = gp->ppi;

	ret = xfs_parent_from_xattr(context->dp->i_mount, flags, name, namelen,
			value, valuelen, &gp->pptr_irec);
	if (ret < 0) {
		xfs_inode_mark_sick(context->dp, XFS_SICK_INO_PARENT);
		context->seen_enough = -EFSCORRUPTED;
		return;
	}
	if (ret != 1)
		return;

	/*
	 * We found a parent pointer, but we've filled up the buffer.  Signal
	 * to the caller that we did /not/ reach the end of the parent pointer
	 * recordset.
	 */
	arraytop = xfs_getparents_arraytop(ppi, ppi->gp_count + 1);
	context->firstu -= xfs_getparents_rec_sizeof(&gp->pptr_irec, namelen);
	if (context->firstu < arraytop) {
		context->seen_enough = 1;
		return;
	}

	/* Format the parent pointer directly into the caller buffer. */
	ppi->gp_offsets[ppi->gp_count] = context->firstu;
	pptr = xfs_getparents_rec(ppi, ppi->gp_count);
	pptr->gpr_ino = gp->pptr_irec.p_ino;
	pptr->gpr_gen = gp->pptr_irec.p_gen;
	pptr->gpr_pad = 0;
	pptr->gpr_rsvd = 0;
	memcpy(pptr->gpr_name, name, namelen);
	pptr->gpr_name[namelen] = 0;

	trace_xfs_getparent_listent(context->dp, ppi, pptr);

	ppi->gp_count++;
}

/* Retrieve the parent pointers for a given inode. */
int
xfs_getparent_pointers(
	struct xfs_inode		*ip,
	struct xfs_getparents		*ppi)
{
	struct xfs_getparent_ctx	*gp;
	int				error;

	if (!xfs_has_parent(ip->i_mount))
		return -EOPNOTSUPP;

	gp = kzalloc(sizeof(struct xfs_getparent_ctx), GFP_KERNEL);
	if (!gp)
		return -ENOMEM;
	gp->ppi = ppi;
	gp->context.dp = ip;
	gp->context.resynch = 1;
	gp->context.put_listent = xfs_getparent_listent;
	gp->context.bufsize = round_down(ppi->gp_bufsize, sizeof(uint32_t));
	gp->context.firstu = gp->context.bufsize;

	/* Copy the cursor provided by caller */
	memcpy(&gp->context.cursor, &ppi->gp_cursor,
			sizeof(struct xfs_attrlist_cursor));
	ppi->gp_count = 0;

	trace_xfs_getparent_pointers(ip, ppi, &gp->context.cursor);

	error = xfs_attr_list(&gp->context);
	if (error)
		goto out_free;
	if (gp->context.seen_enough < 0) {
		error = gp->context.seen_enough;
		goto out_free;
	}

	/* Is this the root directory? */
	if (ip->i_ino == ip->i_mount->m_sb.sb_rootino)
		ppi->gp_flags |= XFS_GETPARENTS_OFLAG_ROOT;

	/*
	 * If we did not run out of buffer space, then we reached the end of
	 * the pptr recordset, so set the DONE flag.
	 */
	if (gp->context.seen_enough == 0)
		ppi->gp_flags |= XFS_GETPARENTS_OFLAG_DONE;

	/* Update the caller with the current cursor position */
	memcpy(&ppi->gp_cursor, &gp->context.cursor,
			sizeof(struct xfs_attrlist_cursor));
out_free:
	kfree(gp);
	return error;
}
