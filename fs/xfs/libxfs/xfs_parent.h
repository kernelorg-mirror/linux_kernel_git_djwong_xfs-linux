// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022-2024 Oracle.
 * All Rights Reserved.
 */
#ifndef	__XFS_PARENT_H__
#define	__XFS_PARENT_H__

/* Metadata validators */
bool xfs_parent_namecheck(unsigned int attr_flags, const void *name,
		size_t length);
bool xfs_parent_valuecheck(struct xfs_mount *mp, const void *value,
		size_t valuelen);

xfs_dahash_t xfs_parent_hashval(struct xfs_mount *mp, const uint8_t *name,
		int namelen, xfs_ino_t parent_ino);
xfs_dahash_t xfs_parent_hashattr(struct xfs_mount *mp, const uint8_t *name,
		int namelen, const void *value, int valuelen);

extern struct kmem_cache	*xfs_parent_args_cache;

/*
 * Dynamically allocd structure used to wrap the needed data to pass around
 * the defer ops machinery
 */
struct xfs_parent_args {
	struct xfs_parent_rec	rec;
	struct xfs_da_args	args;
};

int xfs_parent_args_alloc(struct xfs_mount *mp,
		struct xfs_parent_args **ppargsp);

/*
 * Start a parent pointer update by allocating the context object we need to
 * perform a parent pointer update.
 */
static inline int
xfs_parent_start(
	struct xfs_mount	*mp,
	struct xfs_parent_args	**ppargsp)
{
	*ppargsp = NULL;

	if (xfs_has_parent(mp))
		return xfs_parent_args_alloc(mp, ppargsp);
	return 0;
}

void xfs_parent_addname(struct xfs_trans *tp, struct xfs_parent_args *ppargs,
		struct xfs_inode *dp, const struct xfs_name *parent_name,
		struct xfs_inode *child);

/* Schedule a parent pointer addition. */
static inline void
xfs_parent_add(struct xfs_trans *tp, struct xfs_parent_args *ppargs,
		struct xfs_inode *dp, const struct xfs_name *parent_name,
		struct xfs_inode *child)
{
	if (ppargs)
		xfs_parent_addname(tp, ppargs, dp, parent_name, child);
}

void xfs_parent_args_free(struct xfs_mount *mp, struct xfs_parent_args *ppargs);

/* Finish a parent pointer update by freeing the context object. */
static inline void
xfs_parent_finish(
	struct xfs_mount	*mp,
	struct xfs_parent_args	*ppargs)
{
	if (ppargs)
		xfs_parent_args_free(mp, ppargs);
}

#endif /* __XFS_PARENT_H__ */
