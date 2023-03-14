// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Oracle, Inc.
 * All rights reserved.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_format.h"
#include "xfs_da_format.h"
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_bmap_btree.h"
#include "xfs_inode.h"
#include "xfs_error.h"
#include "xfs_trace.h"
#include "xfs_trans.h"
#include "xfs_da_btree.h"
#include "xfs_attr.h"
#include "xfs_da_btree.h"
#include "xfs_attr_sf.h"
#include "xfs_bmap.h"
#include "xfs_defer.h"
#include "xfs_log.h"
#include "xfs_xattr.h"
#include "xfs_parent.h"
#include "xfs_trans_space.h"

struct kmem_cache		*xfs_parent_intent_cache;

/*
 * Parent pointer attribute handling.
 *
 * Because the attribute value is a filename component, it will never be longer
 * than 255 bytes. This means the attribute will always be a local format
 * attribute as it is xfs_attr_leaf_entsize_local_max() for v5 filesystems will
 * always be larger than this (max is 75% of block size).
 *
 * Creating a new parent attribute will always create a new attribute - there
 * should never, ever be an existing attribute in the tree for a new inode.
 * ENOSPC behavior is problematic - creating the inode without the parent
 * pointer is effectively a corruption, so we allow parent attribute creation
 * to dip into the reserve block pool to avoid unexpected ENOSPC errors from
 * occurring.
 */

/* Return true if parent pointer EA name is valid. */
bool
xfs_parent_namecheck(
	struct xfs_mount			*mp,
	const struct xfs_parent_name_rec	*rec,
	size_t					reclen,
	unsigned int				attr_flags)
{
	xfs_ino_t				p_ino;

	if (!(attr_flags & XFS_ATTR_PARENT))
		return false;

	if (reclen <= sizeof(struct xfs_parent_name_rec) ||
	    reclen > XFS_PARENT_NAME_MAX_SIZE)
		return false;

	/* Only one namespace bit allowed. */
	if (hweight32(attr_flags & XFS_ATTR_NSP_ONDISK_MASK) > 1)
		return false;

	p_ino = be64_to_cpu(rec->p_ino);
	if (!xfs_verify_ino(mp, p_ino))
		return false;

	return true;
}

/* Return true if parent pointer EA value is valid. */
bool
xfs_parent_valuecheck(
	struct xfs_mount		*mp,
	size_t				namelen,
	const void			*value,
	size_t				valuelen)
{
	if (namelen > XFS_PARENT_NAME_MAX_SIZE)
		return false;

	if (namelen < XFS_PARENT_NAME_MAX_SIZE && valuelen != 0)
		return false;

	if (namelen == XFS_PARENT_NAME_MAX_SIZE &&
	    valuelen > XFS_PARENT_MAX_DNAME_VALUELEN)
		return false;

	if (value == NULL)
		return false;

	return true;
}

/*
 * Initializes a xfs_parent_name_rec to be stored as an attribute name.
 * Returns the number of name bytes stored in p_dname.
 */
static inline int
xfs_init_parent_name_rec(
	struct xfs_parent_name_rec	*rec,
	const struct xfs_inode		*dp,
	const struct xfs_name		*name,
	struct xfs_inode		*ip)
{
	int				dnamelen;

	rec->p_ino = cpu_to_be64(dp->i_ino);
	rec->p_gen = cpu_to_be32(VFS_IC(dp)->i_generation);

	dnamelen = min_t(int, name->len, XFS_PARENT_MAX_DNAME_SIZE);
	memcpy(rec->p_dname, name->name, dnamelen);
	return dnamelen;
}

/*
 * Convert an ondisk parent_name xattr to its incore format.  If @value is
 * NULL, set @irec->p_namelen to zero and leave @irec->p_name untouched.
 */
void
xfs_parent_irec_from_disk(
	struct xfs_parent_name_irec	*irec,
	const struct xfs_parent_name_rec *rec,
	int				reclen,
	const void			*value,
	int				valuelen)
{
	int				dnamelen;

	irec->p_ino = be64_to_cpu(rec->p_ino);
	irec->p_gen = be32_to_cpu(rec->p_gen);

	if (!value) {
		irec->p_namelen = 0;
		return;
	}

	ASSERT(valuelen <= XFS_PARENT_MAX_DNAME_VALUELEN);

	dnamelen = xfs_parent_name_dnamelen(reclen);
	irec->p_namelen = dnamelen + valuelen;
	memcpy(irec->p_name, rec->p_dname, dnamelen);
	if (valuelen > 0)
		memcpy(irec->p_name + dnamelen, value, valuelen);
}

/*
 * Convert an incore parent_name record to its ondisk format.  If @valuelen is
 * NULL, neither it nor @value will be written to.
 */
int
xfs_parent_irec_to_disk(
	struct xfs_parent_name_rec	*rec,
	int				*reclen,
	void				*value,
	int				*valuelen,
	const struct xfs_parent_name_irec *irec)
{
	int				dnamelen;

	rec->p_ino = cpu_to_be64(irec->p_ino);
	rec->p_gen = cpu_to_be32(irec->p_gen);
	dnamelen = min_t(int, irec->p_namelen, XFS_PARENT_MAX_DNAME_SIZE);
	*reclen = xfs_parent_name_rec_sizeof(dnamelen);
	memcpy(rec->p_dname, irec->p_name, dnamelen);

	if (!valuelen)
		return dnamelen;

	*valuelen = irec->p_namelen - dnamelen;
	if (*valuelen)
		memcpy(value, rec->p_dname + XFS_PARENT_MAX_DNAME_SIZE,
				*valuelen);

	return dnamelen;
}

/*
 * Allocate memory to control a logged parent pointer update as part of a
 * dirent operation.
 */
int
__xfs_parent_init(
	struct xfs_mount		*mp,
	bool				grab_log,
	struct xfs_parent_defer		**parentp)
{
	struct xfs_parent_defer		*parent;
	int				error;

	if (grab_log) {
		error = xfs_attr_grab_log_assist(mp);
		if (error)
			return error;
	}

	parent = kmem_cache_zalloc(xfs_parent_intent_cache, GFP_KERNEL);
	if (!parent) {
		if (grab_log)
			xfs_attr_rele_log_assist(mp);
		return -ENOMEM;
	}

	/* init parent da_args */
	parent->have_log = grab_log;
	parent->args.geo = mp->m_attr_geo;
	parent->args.whichfork = XFS_ATTR_FORK;
	parent->args.attr_filter = XFS_ATTR_PARENT;
	parent->args.op_flags = XFS_DA_OP_OKNOENT | XFS_DA_OP_LOGGED |
				XFS_DA_OP_VLOOKUP;
	parent->args.name = (const uint8_t *)&parent->rec;
	parent->args.namelen = 0;

	*parentp = parent;
	return 0;
}

/* Add a parent pointer to reflect a dirent addition. */
int
xfs_parent_add(
	struct xfs_trans	*tp,
	struct xfs_parent_defer	*parent,
	struct xfs_inode	*dp,
	const struct xfs_name	*parent_name,
	struct xfs_inode	*child)
{
	struct xfs_da_args	*args = &parent->args;
	int			dnamelen;

	dnamelen = xfs_init_parent_name_rec(&parent->rec, dp, parent_name,
			child);

	args->namelen = xfs_parent_name_rec_sizeof(dnamelen);
	args->hashval = xfs_da_hashname(args->name, args->namelen);

	args->trans = tp;
	args->dp = child;

	parent->args.valuelen = parent_name->len - dnamelen;
	if (parent->args.valuelen > 0)
		parent->args.value = (void *)parent_name->name + dnamelen;
	else
		parent->args.value = NULL;

	return xfs_attr_defer_add(args);
}

/* Remove a parent pointer to reflect a dirent removal. */
int
xfs_parent_remove(
	struct xfs_trans	*tp,
	struct xfs_parent_defer	*parent,
	struct xfs_inode	*dp,
	const struct xfs_name	*name,
	struct xfs_inode	*child)
{
	struct xfs_da_args	*args = &parent->args;
	int			dnamelen;

	dnamelen = xfs_init_parent_name_rec(&parent->rec, dp, name, child);

	args->namelen = xfs_parent_name_rec_sizeof(dnamelen);
	args->trans = tp;
	args->dp = child;
	args->hashval = xfs_da_hashname(args->name, args->namelen);

	parent->args.valuelen = name->len - dnamelen;
	if (parent->args.valuelen > 0)
		parent->args.value = (void *)name->name + dnamelen;
	else
		parent->args.value = NULL;

	return xfs_attr_defer_remove(args);
}

/* Replace one parent pointer with another to reflect a rename. */
int
xfs_parent_replace(
	struct xfs_trans	*tp,
	struct xfs_parent_defer	*new_parent,
	struct xfs_inode	*old_dp,
	const struct xfs_name	*old_name,
	struct xfs_inode	*new_dp,
	const struct xfs_name	*new_name,
	struct xfs_inode	*child)
{
	struct xfs_da_args	*args = &new_parent->args;
	int			old_dnamelen, new_dnamelen;

	old_dnamelen = xfs_init_parent_name_rec(&new_parent->old_rec, old_dp,
			old_name, child);
	new_dnamelen = xfs_init_parent_name_rec(&new_parent->rec, new_dp,
			new_name, child);

	new_parent->args.name = (const uint8_t *)&new_parent->old_rec;
	new_parent->args.namelen = xfs_parent_name_rec_sizeof(old_dnamelen);
	new_parent->args.new_name = (const uint8_t *)&new_parent->rec;
	new_parent->args.new_namelen = xfs_parent_name_rec_sizeof(new_dnamelen);
	args->trans = tp;
	args->dp = child;

	new_parent->args.new_valuelen = new_name->len - new_dnamelen;
	if (new_parent->args.new_valuelen > 0)
		new_parent->args.new_value = (void *)new_name->name + new_dnamelen;
	else
		new_parent->args.new_value = NULL;

	new_parent->args.valuelen = old_name->len - old_dnamelen;
	if (new_parent->args.valuelen > 0)
		new_parent->args.value = (void *)old_name->name + old_dnamelen;
	else
		new_parent->args.value = NULL;

	args->hashval = xfs_da_hashname(args->name, args->namelen);
	return xfs_attr_defer_replace(args);
}

void
__xfs_parent_cancel(
	xfs_mount_t		*mp,
	struct xfs_parent_defer *parent)
{
	if (parent->have_log)
		xlog_drop_incompat_feat(mp->m_log);
	kmem_cache_free(xfs_parent_intent_cache, parent);
}

unsigned int
xfs_pptr_calc_space_res(
	struct xfs_mount	*mp,
	unsigned int		namelen)
{
	/*
	 * Pptrs are always the first attr in an attr tree, and never larger
	 * than a block
	 */
	return XFS_DAENTER_SPACE_RES(mp, XFS_ATTR_FORK) +
	       XFS_NEXTENTADD_SPACE_RES(mp, namelen, XFS_ATTR_FORK);
}

/*
 * Look up the @name associated with the parent pointer (@pptr) of @ip.
 * Caller must hold at least ILOCK_SHARED.  Returns 0 if the pointer is found,
 * -ENOATTR if there is no match, or a negative errno.  The scratchpad need not
 *  be initialized.
 */
int
xfs_parent_lookup(
	struct xfs_trans		*tp,
	struct xfs_inode		*ip,
	const struct xfs_parent_name_irec *pptr,
	struct xfs_parent_scratch	*scr)
{
	int				dnamelen;
	int				reclen;

	dnamelen = xfs_parent_irec_to_disk(&scr->rec, &reclen, NULL, NULL, pptr);

	memset(&scr->args, 0, sizeof(struct xfs_da_args));
	scr->args.attr_filter	= XFS_ATTR_PARENT;
	scr->args.dp		= ip;
	scr->args.geo		= ip->i_mount->m_attr_geo;
	scr->args.name		= (const unsigned char *)&scr->rec;
	scr->args.namelen	= reclen;
	scr->args.op_flags	= XFS_DA_OP_OKNOENT | XFS_DA_OP_VLOOKUP;
	scr->args.trans		= tp;
	scr->args.valuelen	= pptr->p_namelen - dnamelen;
	scr->args.whichfork	= XFS_ATTR_FORK;

	if (scr->args.valuelen)
		scr->args.value	= (void *)pptr->p_name + dnamelen;

	scr->args.hashval = xfs_da_hashname(scr->args.name, scr->args.namelen);

	return xfs_attr_get_ilocked(&scr->args);
}

/*
 * Attach the parent pointer (@pptr -> @name) to @ip immediately.  Caller must
 * not have a transaction or hold the ILOCK.  The update will not use logged
 * xattrs.  This is for specialized repair functions only.  The scratchpad need
 * not be initialized.
 */
int
xfs_parent_set(
	struct xfs_inode		*ip,
	const struct xfs_parent_name_irec *pptr,
	struct xfs_parent_scratch	*scr)
{
	int				dnamelen;
	int				reclen;

	dnamelen = xfs_parent_irec_to_disk(&scr->rec, &reclen, NULL, NULL, pptr);

	memset(&scr->args, 0, sizeof(struct xfs_da_args));
	scr->args.attr_filter	= XFS_ATTR_PARENT;
	scr->args.dp		= ip;
	scr->args.geo		= ip->i_mount->m_attr_geo;
	scr->args.name		= (const unsigned char *)&scr->rec;
	scr->args.namelen	= reclen;
	scr->args.op_flags	= XFS_DA_OP_VLOOKUP;
	scr->args.valuelen	= pptr->p_namelen - dnamelen;
	scr->args.whichfork	= XFS_ATTR_FORK;

	if (scr->args.valuelen)
		scr->args.value	= (void *)pptr->p_name + dnamelen;

	return xfs_attr_set(&scr->args);
}

/*
 * Remove the parent pointer (@rec -> @name) from @ip immediately.  Caller must
 * not have a transaction or hold the ILOCK.  The update will not use logged
 * xattrs.  This is for specialized repair functions only.  The scratchpad need
 * not be initialized.
 */
int
xfs_parent_unset(
	struct xfs_inode		*ip,
	const struct xfs_parent_name_irec *pptr,
	struct xfs_parent_scratch	*scr)
{
	int				dnamelen;
	int				reclen;

	dnamelen = xfs_parent_irec_to_disk(&scr->rec, &reclen, NULL, NULL, pptr);

	memset(&scr->args, 0, sizeof(struct xfs_da_args));
	scr->args.attr_filter	= XFS_ATTR_PARENT;
	scr->args.dp		= ip;
	scr->args.geo		= ip->i_mount->m_attr_geo;
	scr->args.name		= (const unsigned char *)&scr->rec;
	scr->args.namelen	= reclen;
	scr->args.op_flags	= XFS_DA_OP_REMOVE | XFS_DA_OP_VLOOKUP;
	scr->args.valuelen	= pptr->p_namelen - dnamelen;
	scr->args.whichfork	= XFS_ATTR_FORK;

	if (scr->args.valuelen)
		scr->args.value	= (void *)pptr->p_name + dnamelen;

	return xfs_attr_set(&scr->args);
}
