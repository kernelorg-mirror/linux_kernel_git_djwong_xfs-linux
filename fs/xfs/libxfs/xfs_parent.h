// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022-2023 Oracle, Inc.
 * All Rights Reserved.
 */
#ifndef	__XFS_PARENT_H__
#define	__XFS_PARENT_H__

/* Metadata validators */
bool xfs_parent_namecheck(struct xfs_mount *mp,
		const struct xfs_parent_name_rec *rec, size_t reclen,
		unsigned int attr_flags);
bool xfs_parent_valuecheck(struct xfs_mount *mp, const void *value,
		size_t valuelen);

extern struct kmem_cache	*xfs_parent_intent_cache;

/*
 * Dynamically allocd structure used to wrap the needed data to pass around
 * the defer ops machinery
 */
struct xfs_parent_defer {
	struct xfs_parent_name_rec	rec;
	struct xfs_parent_name_rec	new_rec;
	struct xfs_da_args		args;
};

int __xfs_parent_init(struct xfs_mount *mp, struct xfs_parent_defer **parentp);

static inline int
xfs_parent_start(
	struct xfs_mount	*mp,
	struct xfs_parent_defer	**pp)
{
	*pp = NULL;

	if (xfs_has_parent(mp))
		return __xfs_parent_init(mp, pp);
	return 0;
}

int xfs_parent_add(struct xfs_trans *tp, struct xfs_parent_defer *parent,
		struct xfs_inode *dp, const struct xfs_name *parent_name,
		struct xfs_inode *child);
int xfs_parent_remove(struct xfs_trans *tp, struct xfs_parent_defer *parent,
		struct xfs_inode *dp, const struct xfs_name *parent_name,
		struct xfs_inode *child);
int xfs_parent_replace(struct xfs_trans *tp, struct xfs_parent_defer *parent,
		struct xfs_inode *old_dp, const struct xfs_name *old_name,
		struct xfs_inode *new_dp, const struct xfs_name *new_name,
		struct xfs_inode *child);

void __xfs_parent_cancel(struct xfs_mount *mp, struct xfs_parent_defer *parent);

static inline void
xfs_parent_finish(
	struct xfs_mount	*mp,
	struct xfs_parent_defer	*p)
{
	if (p)
		__xfs_parent_cancel(mp, p);
}

/*
 * Incore version of a parent pointer, also contains dirent name so callers
 * can pass/obtain all the parent pointer information in a single structure
 */
struct xfs_parent_name_irec {
	/* Parent pointer attribute name fields */
	xfs_ino_t		p_ino;
	uint32_t		p_gen;
	xfs_dahash_t		p_namehash;

	/* Parent pointer attribute value fields */
	uint8_t			p_namelen;
	unsigned char		p_name[MAXNAMELEN];
};

void xfs_parent_irec_from_disk(struct xfs_parent_name_irec *irec,
		const struct xfs_parent_name_rec *rec, const void *value,
		unsigned int valuelen);
void xfs_parent_irec_to_disk(struct xfs_parent_name_rec *rec,
		const struct xfs_parent_name_irec *irec);
void xfs_parent_irec_hashname(struct xfs_mount *mp,
		struct xfs_parent_name_irec *irec);

/* Scratchpad memory so that raw parent operations don't burn stack space. */
struct xfs_parent_scratch {
	struct xfs_parent_name_rec	rec;
	struct xfs_da_args		args;
};

int xfs_parent_lookup(struct xfs_trans *tp, struct xfs_inode *ip,
		const struct xfs_parent_name_irec *pptr,
		struct xfs_parent_scratch *scratch);

int xfs_parent_set(struct xfs_inode *ip,
		const struct xfs_parent_name_irec *pptr,
		struct xfs_parent_scratch *scratch);

int xfs_parent_unset(struct xfs_inode *ip,
		const struct xfs_parent_name_irec *rec,
		struct xfs_parent_scratch *scratch);

#endif	/* __XFS_PARENT_H__ */
