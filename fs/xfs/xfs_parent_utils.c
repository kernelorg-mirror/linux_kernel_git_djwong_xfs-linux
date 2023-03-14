// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Oracle, Inc.
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

struct xfs_getparent_ctx {
	struct xfs_attr_list_context	context;
	struct xfs_parent_name_irec	pptr_irec;
	struct xfs_pptr_info		*ppi;
};

static inline unsigned int
xfs_getparents_rec_sizeof(
	const struct xfs_parent_name_irec	*irec)
{
	return round_up(sizeof(struct xfs_parent_ptr) + irec->p_namelen + 1,
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
	struct xfs_pptr_info		*ppi;
	struct xfs_parent_ptr		*pptr;
	struct xfs_parent_name_irec	*irec;
	struct xfs_mount		*mp = context->dp->i_mount;
	int				arraytop;

	gp = container_of(context, struct xfs_getparent_ctx, context);
	ppi = gp->ppi;
	irec = &gp->pptr_irec;

	/* Ignore non-parent xattrs */
	if (!(flags & XFS_ATTR_PARENT))
		return;

	/*
	 * Report corruption for xattrs with any other flag set, or for a
	 * parent pointer that has a remote value.  The attr list functions
	 * filtered any INCOMPLETE attrs for us.
	 */
	if (XFS_IS_CORRUPT(mp,
			   hweight32(flags & XFS_ATTR_NSP_ONDISK_MASK) > 1) ||
	    XFS_IS_CORRUPT(mp, value == NULL)) {
		context->seen_enough = -EFSCORRUPTED;
		return;
	}

	xfs_parent_irec_from_disk(&gp->pptr_irec, (void *)name, value,
			valuelen);

	/*
	 * We found a parent pointer, but we've filled up the buffer.  Signal
	 * to the caller that we did /not/ reach the end of the parent pointer
	 * recordset.
	 */
	arraytop = xfs_getparents_arraytop(ppi, ppi->pi_count + 1);
	context->firstu -= xfs_getparents_rec_sizeof(irec);
	if (context->firstu < arraytop) {
		context->seen_enough = 1;
		return;
	}

	/* Format the parent pointer directly into the caller buffer. */
	ppi->pi_offsets[ppi->pi_count] = context->firstu;
	pptr = xfs_ppinfo_to_pp(ppi, ppi->pi_count);
	pptr->xpp_ino = irec->p_ino;
	pptr->xpp_gen = irec->p_gen;
	pptr->xpp_diroffset = irec->p_diroffset;
	pptr->xpp_rsvd = 0;

	memcpy(pptr->xpp_name, irec->p_name, irec->p_namelen);
	pptr->xpp_name[irec->p_namelen] = 0;
	ppi->pi_count++;
}

/* Retrieve the parent pointers for a given inode. */
int
xfs_getparent_pointers(
	struct xfs_inode		*ip,
	struct xfs_pptr_info		*ppi)
{
	struct xfs_getparent_ctx	*gp;
	int				error;

	gp = kzalloc(sizeof(struct xfs_getparent_ctx), GFP_KERNEL);
	if (!gp)
		return -ENOMEM;
	gp->ppi = ppi;
	gp->context.dp = ip;
	gp->context.resynch = 1;
	gp->context.put_listent = xfs_getparent_listent;
	gp->context.bufsize = round_down(ppi->pi_ptrs_size, sizeof(uint32_t));
	gp->context.firstu = gp->context.bufsize;

	/* Copy the cursor provided by caller */
	memcpy(&gp->context.cursor, &ppi->pi_cursor,
			sizeof(struct xfs_attrlist_cursor));
	ppi->pi_count = 0;

	error = xfs_attr_list(&gp->context);
	if (error)
		goto out_free;
	if (gp->context.seen_enough < 0) {
		error = gp->context.seen_enough;
		goto out_free;
	}

	/* Is this the root directory? */
	if (ip->i_ino == ip->i_mount->m_sb.sb_rootino)
		ppi->pi_flags |= XFS_PPTR_OFLAG_ROOT;

	/*
	 * If we did not run out of buffer space, then we reached the end of
	 * the pptr recordset, so set the DONE flag.
	 */
	if (gp->context.seen_enough == 0)
		ppi->pi_flags |= XFS_PPTR_OFLAG_DONE;

	/* Update the caller with the current cursor position */
	memcpy(&ppi->pi_cursor, &gp->context.cursor,
			sizeof(struct xfs_attrlist_cursor));
out_free:
	kfree(gp);
	return error;
}

