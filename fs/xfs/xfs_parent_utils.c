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

/*
 * Get the parent pointers for a given inode
 *
 * Returns 0 on success and non zero on error
 */
int
xfs_attr_get_parent_pointer(
	struct xfs_inode		*ip,
	struct xfs_pptr_info		*ppi)
{

	struct xfs_attrlist		*alist;
	struct xfs_attrlist_ent		*aent;
	struct xfs_parent_ptr		*xpp;
	struct xfs_parent_name_rec	*xpnr;
	char				*namebuf;
	unsigned int			namebuf_size;
	int				name_len, i, error = 0;
	unsigned int			lock_mode, flags = XFS_ATTR_PARENT;
	struct xfs_attr_list_context	context;

	/* Allocate a buffer to store the attribute names */
	namebuf_size = sizeof(struct xfs_attrlist) +
		       (ppi->pi_ptrs_size) * sizeof(struct xfs_attrlist_ent);
	namebuf = kvzalloc(namebuf_size, GFP_KERNEL);
	if (!namebuf)
		return -ENOMEM;

	memset(&context, 0, sizeof(struct xfs_attr_list_context));
	error = xfs_ioc_attr_list_context_init(ip, namebuf, namebuf_size, 0,
			&context);
	if (error)
		goto out_kfree;

	/* Copy the cursor provided by caller */
	memcpy(&context.cursor, &ppi->pi_cursor,
		sizeof(struct xfs_attrlist_cursor));
	context.attr_filter = XFS_ATTR_PARENT;

	lock_mode = xfs_ilock_attr_map_shared(ip);

	error = xfs_attr_list_ilocked(&context);
	if (error)
		goto out_unlock;

	alist = (struct xfs_attrlist *)namebuf;
	for (i = 0; i < alist->al_count; i++) {
		struct xfs_da_args args = {
			.geo = ip->i_mount->m_attr_geo,
			.whichfork = XFS_ATTR_FORK,
			.dp = ip,
			.namelen = sizeof(struct xfs_parent_name_rec),
			.attr_filter = flags,
		};

		xpp = xfs_ppinfo_to_pp(ppi, i);
		memset(xpp, 0, sizeof(struct xfs_parent_ptr));
		aent = (struct xfs_attrlist_ent *)
			&namebuf[alist->al_offset[i]];
		xpnr = (struct xfs_parent_name_rec *)(aent->a_name);

		if (aent->a_valuelen > XFS_PPTR_MAXNAMELEN) {
			error = -EFSCORRUPTED;
			goto out_unlock;
		}
		name_len = aent->a_valuelen;

		args.name = (char *)xpnr;
		args.hashval = xfs_da_hashname(args.name, args.namelen),
		args.value = (unsigned char *)(xpp->xpp_name);
		args.valuelen = name_len;

		error = xfs_attr_get_ilocked(&args);
		error = (error == -EEXIST ? 0 : error);
		if (error) {
			error = -EFSCORRUPTED;
			goto out_unlock;
		}

		xfs_init_parent_ptr(xpp, xpnr);
		if (!xfs_verify_ino(args.dp->i_mount, xpp->xpp_ino)) {
			error = -EFSCORRUPTED;
			goto out_unlock;
		}
	}
	ppi->pi_ptrs_used = alist->al_count;
	if (!alist->al_more)
		ppi->pi_flags |= XFS_PPTR_OFLAG_DONE;

	/* Update the caller with the current cursor position */
	memcpy(&ppi->pi_cursor, &context.cursor,
			sizeof(struct xfs_attrlist_cursor));

out_unlock:
	xfs_iunlock(ip, lock_mode);
out_kfree:
	kvfree(namebuf);

	return error;
}

