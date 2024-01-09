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
#include "xfs_handle.h"
#include "xfs_health.h"
#include "xfs_icache.h"

struct xfs_getparent_ctx {
	struct xfs_attr_list_context	context;
	struct xfs_getparents		*ppi;
};

static inline unsigned int
xfs_getparents_rec_sizeof(
	unsigned int		namelen)
{
	return round_up(sizeof(struct xfs_getparents_rec) + namelen + 1,
			sizeof(uint32_t));
}

static void
xfs_handle_init(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	uint32_t		gen,
	struct xfs_handle	*handle)
{
	memcpy(&handle->ha_fsid, mp->m_fixedfsid, sizeof(xfs_fsid_t));

	handle->ha_fid.fid_len = sizeof(struct xfs_fid) -
				 sizeof(handle->ha_fid.fid_len);
	handle->ha_fid.fid_pad = 0;
	handle->ha_fid.fid_gen = gen;
	handle->ha_fid.fid_ino = ino;
}

static inline unsigned int
xfs_getparents_arraytop(
	const struct xfs_getparents	*ppi,
	unsigned int			nr)
{
	return sizeof(struct xfs_getparents) +
			(nr * sizeof(ppi->gp_offsets[0]));
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
	struct xfs_inode		*ip = context->dp;
	struct xfs_mount		*mp = ip->i_mount;
	xfs_ino_t			ino;
	uint32_t			gen;
	unsigned short			reclen = xfs_getparents_rec_sizeof(namelen);
	int				arraytop;
	int				ret;

	gp = container_of(context, struct xfs_getparent_ctx, context);
	ppi = gp->ppi;

	ret = xfs_parent_from_xattr(mp, flags, name, namelen, value, valuelen,
			&ino, &gen);
	if (ret < 0) {
		xfs_inode_mark_sick(ip, XFS_SICK_INO_PARENT);
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
	context->firstu -= reclen;
	if (context->firstu < arraytop) {
		context->seen_enough = 1;
		return;
	}

	/* Format the parent pointer directly into the caller buffer. */
	ppi->gp_offsets[ppi->gp_count] = context->firstu;
	pptr = xfs_getparents_rec(ppi, ppi->gp_count);
	pptr->gpr_reclen = reclen;
	xfs_handle_init(mp, ino, gen, &pptr->gpr_parent);
	memcpy(pptr->gpr_name, name, namelen);
	pptr->gpr_name[namelen] = 0;

	trace_xfs_getparent_listent(ip, ppi, pptr);

	ppi->gp_count++;
}

/* Retrieve the parent pointers for a given inode. */
STATIC int
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

/*
 * IOCTL routine to get the parent pointers of an inode and return it to user
 * space.  Caller must pass a buffer space containing a struct xfs_getparents,
 * followed by a region large enough to contain an array of struct
 * xfs_getparents_rec of a size specified in gp_bufsize.  If the inode contains
 * more parent pointers than can fit in the buffer space, caller may re-call
 * the function using the returned gp_cursor to resume iteration.  The
 * number of xfs_getparents_rec returned will be stored in gp_count.
 *
 * Returns 0 on success or non-zero on failure
 */
int
xfs_ioc_get_parent_pointer(
	struct file			*filp,
	void				__user *arg)
{
	struct xfs_getparents		*ppi = NULL;
	int				error = 0;
	struct xfs_inode		*file_ip = XFS_I(file_inode(filp));
	struct xfs_inode		*call_ip = file_ip;
	struct xfs_mount		*mp = file_ip->i_mount;
	void				__user *o_pptr;
	struct xfs_getparents_rec	*i_pptr;
	unsigned int			bytes;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	/* Allocate an xfs_getparents to put the user data */
	ppi = kvmalloc(sizeof(struct xfs_getparents), GFP_KERNEL);
	if (!ppi)
		return -ENOMEM;

	/* Copy the data from the user */
	error = copy_from_user(ppi, arg, sizeof(struct xfs_getparents));
	if (error) {
		error = -EFAULT;
		goto out;
	}

	/* Check size of buffer requested by user */
	if (ppi->gp_bufsize > XFS_XATTR_LIST_MAX) {
		error = -ENOMEM;
		goto out;
	}
	if (ppi->gp_bufsize < sizeof(struct xfs_getparents)) {
		error = -EINVAL;
		goto out;
	}

	if (ppi->gp_flags & ~XFS_GETPARENTS_FLAG_ALL) {
		error = -EINVAL;
		goto out;
	}
	ppi->gp_flags &= ~(XFS_GETPARENTS_OFLAG_ROOT | XFS_GETPARENTS_OFLAG_DONE);

	/*
	 * Now that we know how big the trailing buffer is, expand
	 * our kernel xfs_getparents to be the same size
	 */
	ppi = kvrealloc(ppi, sizeof(struct xfs_getparents), ppi->gp_bufsize,
			GFP_KERNEL | __GFP_ZERO);
	if (!ppi)
		return -ENOMEM;

	if (memchr_inv(&ppi->gp_handle, 0, sizeof(ppi->gp_handle))) {
		struct xfs_handle	*hanp = &ppi->gp_handle;

		if (memcmp(&hanp->ha_fsid, mp->m_fixedfsid,
							sizeof(xfs_fsid_t))) {
			error = -EINVAL;
			goto out;
		}

		if (hanp->ha_fid.fid_ino != file_ip->i_ino) {
			error = xfs_iget(mp, NULL, hanp->ha_fid.fid_ino,
					XFS_IGET_UNTRUSTED, 0, &call_ip);
			if (error)
				goto out;

			/*
			 * Reload the incore unlinked list to avoid failure in
			 * inodegc.  Use an unlocked check here because
			 * unrecovered unlinked inodes should be somewhat rare.
			 */
			if (xfs_inode_unlinked_incomplete(call_ip)) {
				error = xfs_inode_reload_unlinked(call_ip);
				if (error)
					goto out;
			}
		}

		if (VFS_I(call_ip)->i_generation != hanp->ha_fid.fid_gen) {
			error = -EINVAL;
			goto out;
		}
	}

	/* Get the parent pointers */
	error = xfs_getparent_pointers(call_ip, ppi);
	if (error)
		goto out;

	/*
	 * If we ran out of buffer space before copying any parent pointers at
	 * all, the caller's buffer was too short.  Tell userspace that, erm,
	 * the message is too long.
	 */
	if (ppi->gp_count == 0 && !(ppi->gp_flags & XFS_GETPARENTS_OFLAG_DONE)) {
		error = -EMSGSIZE;
		goto out;
	}

	/* Copy the parent pointer head back to the user */
	bytes = xfs_getparents_arraytop(ppi, ppi->gp_count);
	error = copy_to_user(arg, ppi, bytes);
	if (error) {
		error = -EFAULT;
		goto out;
	}

	if (ppi->gp_count == 0)
		goto out;

	/* Copy the parent pointer records back to the user. */
	o_pptr = (__user char*)arg + ppi->gp_offsets[ppi->gp_count - 1];
	i_pptr = xfs_getparents_rec(ppi, ppi->gp_count - 1);
	bytes = ((char *)ppi + ppi->gp_bufsize) - (char *)i_pptr;
	error = copy_to_user(o_pptr, i_pptr, bytes);
	if (error) {
		error = -EFAULT;
		goto out;
	}

out:
	if (call_ip != file_ip)
		xfs_irele(call_ip);
	kvfree(ppi);
	return error;
}
