// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022-2024 Oracle.
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
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "xfs_attr_sf.h"
#include "xfs_bmap.h"
#include "xfs_defer.h"
#include "xfs_log.h"
#include "xfs_xattr.h"
#include "xfs_parent.h"
#include "xfs_trans_space.h"
#include "xfs_attr_item.h"
#include "xfs_health.h"

struct kmem_cache		*xfs_parent_args_cache;

/*
 * Parent pointer attribute handling.
 *
 * Because the attribute name is a filename component, it will never be longer
 * than 255 bytes and must not contain nulls or slashes.  These are roughly the
 * same constraints that apply to attribute names.
 *
 * The attribute value must always be a struct xfs_parent_rec.  This means the
 * attribute will never be in remote format because 12 bytes is nowhere near
 * xfs_attr_leaf_entsize_local_max() (~75% of block size).
 *
 * Creating a new parent attribute will always create a new attribute - there
 * should never, ever be an existing attribute in the tree for a new inode.
 * ENOSPC behavior is problematic - creating the inode without the parent
 * pointer is effectively a corruption, so we allow parent attribute creation
 * to dip into the reserve block pool to avoid unexpected ENOSPC errors from
 * occurring.
 */

/* Return true if parent pointer attr name is valid. */
bool
xfs_parent_namecheck(
	unsigned int			attr_flags,
	const void			*name,
	size_t				length)
{
	/*
	 * Parent pointers always use logged operations, so there should never
	 * be incomplete xattrs.
	 */
	if (attr_flags & XFS_ATTR_INCOMPLETE)
		return false;

	return xfs_dir2_namecheck(name, length);
}

/* Return true if parent pointer attr value is valid. */
bool
xfs_parent_valuecheck(
	struct xfs_mount		*mp,
	const void			*value,
	size_t				valuelen)
{
	const struct xfs_parent_rec	*rec = value;

	if (!xfs_has_parent(mp))
		return false;

	/* The xattr value must be a parent record. */
	if (valuelen != sizeof(struct xfs_parent_rec))
		return false;

	/* The parent record must be local. */
	if (value == NULL)
		return false;

	/* The parent inumber must be valid. */
	if (!xfs_verify_dir_ino(mp, be64_to_cpu(rec->p_ino)))
		return false;

	return true;
}

/* Compute the attribute name hash for a parent pointer. */
xfs_dahash_t
xfs_parent_hashval(
	struct xfs_mount		*mp,
	const uint8_t			*name,
	int				namelen,
	xfs_ino_t			parent_ino)
{
	struct xfs_name			xname = {
		.name			= name,
		.len			= namelen,
	};
	xfs_dahash_t			ret;

	/*
	 * Use the same dirent name hash as would be used on the directory, but
	 * mix in the parent inode number.
	 */
	ret = xfs_dir2_hashname(mp, &xname);
	ret ^= upper_32_bits(parent_ino);
	ret ^= lower_32_bits(parent_ino);
	return ret;
}

/* Compute the attribute name hash from the xattr components. */
xfs_dahash_t
xfs_parent_hashattr(
	struct xfs_mount		*mp,
	const uint8_t			*name,
	int				namelen,
	const void			*value,
	int				valuelen)
{
	const struct xfs_parent_rec	*rec = value;

	/* Requires a local attr value in xfs_parent_rec format */
	if (valuelen != sizeof(struct xfs_parent_rec)) {
		ASSERT(valuelen == sizeof(struct xfs_parent_rec));
		return 0;
	}

	if (!value) {
		ASSERT(value != NULL);
		return 0;
	}

	return xfs_parent_hashval(mp, name, namelen, be64_to_cpu(rec->p_ino));
}

/* Initializes a xfs_parent_rec to be stored as an attribute name. */
static inline void
xfs_parent_rec_init(
	struct xfs_parent_rec		*rec,
	const struct xfs_inode		*dp)
{
	rec->p_ino = cpu_to_be64(dp->i_ino);
	rec->p_gen = cpu_to_be32(VFS_IC(dp)->i_generation);
}

/*
 * Initialize the parent pointer arguments structure.  Caller must have zeroed
 * the contents of @args.  @tp is only required for updates.
 */
static void
xfs_parent_da_args_init(
	struct xfs_da_args	*args,
	struct xfs_trans	*tp,
	struct xfs_parent_rec	*rec,
	struct xfs_inode	*child,
	xfs_ino_t		owner,
	const unsigned char	*parent_name,
	unsigned int		parent_namelen)
{
	args->geo = child->i_mount->m_attr_geo;
	args->whichfork = XFS_ATTR_FORK;
	args->attr_filter = XFS_ATTR_PARENT;
	args->op_flags = XFS_DA_OP_LOGGED | XFS_DA_OP_OKNOENT;
	args->trans = tp;
	args->dp = child;
	args->owner = owner;
	args->name = parent_name;
	args->namelen = parent_namelen;
	args->value = rec;
	args->valuelen = sizeof(struct xfs_parent_rec);
	xfs_attr_sethash(args);
}

/*
 * Set up a parent pointer add, lookup, or remove operation; or the first
 * step of a replace operation.
 */
static void
xfs_parent_args_setname(
	struct xfs_parent_args	*ppargs,
	struct xfs_trans	*tp,
	struct xfs_inode	*dp,
	const struct xfs_name	*parent_name,
	struct xfs_inode	*child)
{
	xfs_parent_rec_init(&ppargs->rec, dp);
	xfs_parent_da_args_init(&ppargs->args, tp, &ppargs->rec, child,
			child->i_ino, parent_name->name, parent_name->len);
}

/* Set up the second step of a replace operation. */
static void
xfs_parent_args_setnewname(
	struct xfs_parent_args	*ppargs,
	struct xfs_trans	*tp,
	struct xfs_inode	*dp,
	const struct xfs_name	*parent_name,
	struct xfs_inode	*child)
{
	xfs_parent_rec_init(&ppargs->new_rec, dp);
	ppargs->args.new_name = parent_name->name;
	ppargs->args.new_namelen = parent_name->len;
	ppargs->args.new_value = &ppargs->new_rec;
	ppargs->args.new_valuelen = sizeof(struct xfs_parent_rec);
}

/* Add a parent pointer to reflect a dirent addition. */
void
xfs_parent_addname(
	struct xfs_trans	*tp,
	struct xfs_parent_args	*ppargs,
	struct xfs_inode	*dp,
	const struct xfs_name	*parent_name,
	struct xfs_inode	*child)
{
	xfs_parent_args_setname(ppargs, tp, dp, parent_name, child);
	xfs_attr_defer_parent(&ppargs->args, XFS_ATTR_DEFER_SET);
}

/* Remove a parent pointer to reflect a dirent removal. */
int
xfs_parent_removename(
	struct xfs_trans	*tp,
	struct xfs_parent_args	*ppargs,
	struct xfs_inode	*dp,
	const struct xfs_name	*parent_name,
	struct xfs_inode	*child)
{
	/*
	 * For regular attrs, removing an attr from a !hasattr inode is a nop.
	 * For parent pointers, we require that the pointer must exist if the
	 * caller wants us to remove the pointer.
	 */
	if (XFS_IS_CORRUPT(child->i_mount, !xfs_inode_hasattr(child))) {
		xfs_inode_mark_sick(child, XFS_SICK_INO_PARENT);
		return -EFSCORRUPTED;
	}

	xfs_parent_args_setname(ppargs, tp, dp, parent_name, child);
	xfs_attr_defer_parent(&ppargs->args, XFS_ATTR_DEFER_REMOVE);
	return 0;
}

/* Replace one parent pointer with another to reflect a rename. */
int
xfs_parent_replacename(
	struct xfs_trans	*tp,
	struct xfs_parent_args	*ppargs,
	struct xfs_inode	*old_dp,
	const struct xfs_name	*old_name,
	struct xfs_inode	*new_dp,
	const struct xfs_name	*new_name,
	struct xfs_inode	*child)
{
	struct xfs_da_args	*args = &ppargs->args;

	/*
	 * For regular attrs, replacing an attr from a !hasattr inode becomes
	 * an attr-set operation.  For replacing a parent pointer, however, we
	 * require that the old pointer must exist.
	 */
	if (XFS_IS_CORRUPT(child->i_mount, !xfs_inode_hasattr(child))) {
		xfs_inode_mark_sick(child, XFS_SICK_INO_PARENT);
		return -EFSCORRUPTED;
	}

	xfs_parent_args_setname(ppargs, tp, old_dp, old_name, child);
	xfs_parent_args_setnewname(ppargs, tp, new_dp, new_name, child);
	xfs_attr_defer_parent(args, XFS_ATTR_DEFER_REPLACE);
	return 0;
}

/* Convert an ondisk parent pointer to the incore format. */
static void
xfs_parent_irec_from_disk(
	struct xfs_parent_irec		*irec,
	const void			*value,
	unsigned int			valuelen)
{
	const struct xfs_parent_rec	*rec = value;

	irec->p_ino = be64_to_cpu(rec->p_ino);
	irec->p_gen = be32_to_cpu(rec->p_gen);
}

/*
 * Extract parent pointer information from any xattr into @irec.  Returns 0 if
 * this is not a parent pointer xattr at all; 1 if this is a valid parent
 * pointer and @irec has been filled out; or -EFSCORRUPTED for garbage.
 */
int
xfs_parent_from_xattr(
	struct xfs_mount	*mp,
	unsigned int		attr_flags,
	const unsigned char	*name,
	unsigned int		namelen,
	const void		*value,
	unsigned int		valuelen,
	struct xfs_parent_irec	*irec)
{
	if (!(attr_flags & XFS_ATTR_PARENT))
		return 0;

	if (!xfs_parent_namecheck(attr_flags, name, namelen))
		return -EFSCORRUPTED;
	if (!xfs_parent_valuecheck(mp, value, valuelen))
		return -EFSCORRUPTED;

	xfs_parent_irec_from_disk(irec, value, valuelen);
	return 1;
}
