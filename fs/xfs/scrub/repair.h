// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SCRUB_REPAIR_H__
#define __XFS_SCRUB_REPAIR_H__

#include "scrub/bitmap.h"
#include "xfs_btree.h"
union xfs_btree_ptr;

static inline int xrep_notsupported(struct xfs_scrub *sc)
{
	return -EOPNOTSUPP;
}

#ifdef CONFIG_XFS_ONLINE_REPAIR

/* Repair helpers */

int xrep_attempt(struct xfs_inode *ip, struct xfs_scrub *sc);
void xrep_failure(struct xfs_mount *mp);
int xrep_roll_ag_trans(struct xfs_scrub *sc);
bool xrep_ag_has_space(struct xfs_perag *pag, xfs_extlen_t nr_blocks,
		enum xfs_ag_resv_type type);
xfs_extlen_t xrep_calc_ag_resblks(struct xfs_scrub *sc);
int xrep_alloc_ag_block(struct xfs_scrub *sc,
		const struct xfs_owner_info *oinfo, xfs_fsblock_t *fsbno,
		enum xfs_ag_resv_type resv);
int xrep_init_btblock(struct xfs_scrub *sc, xfs_fsblock_t fsb,
		struct xfs_buf **bpp, xfs_btnum_t btnum,
		const struct xfs_buf_ops *ops);

struct xbitmap;

int xrep_fix_freelist(struct xfs_scrub *sc, bool can_shrink);
int xrep_reap_extents(struct xfs_scrub *sc, struct xbitmap *exlist,
		const struct xfs_owner_info *oinfo, enum xfs_ag_resv_type type);

struct xrep_find_ag_btree {
	/* in: rmap owner of the btree we're looking for */
	uint64_t			rmap_owner;

	/* in: buffer ops */
	const struct xfs_buf_ops	*buf_ops;

	/* out: the highest btree block found and the tree height */
	xfs_agblock_t			root;
	unsigned int			height;
};

int xrep_find_ag_btree_roots(struct xfs_scrub *sc, struct xfs_buf *agf_bp,
		struct xrep_find_ag_btree *btree_info, struct xfs_buf *agfl_bp);
void xrep_force_quotacheck(struct xfs_scrub *sc, uint dqtype);
int xrep_ino_dqattach(struct xfs_scrub *sc);
int xrep_reset_perag_resv(struct xfs_scrub *sc);

/* Metadata revalidators */

int xrep_revalidate_allocbt(struct xfs_scrub *sc);
int xrep_revalidate_iallocbt(struct xfs_scrub *sc);

/* Metadata repairers */

int xrep_probe(struct xfs_scrub *sc);
int xrep_superblock(struct xfs_scrub *sc);
int xrep_agf(struct xfs_scrub *sc);
int xrep_agfl(struct xfs_scrub *sc);
int xrep_agi(struct xfs_scrub *sc);
int xrep_allocbt(struct xfs_scrub *sc);
int xrep_iallocbt(struct xfs_scrub *sc);

struct xrep_newbt_resv {
	/* Link to list of extents that we've reserved. */
	struct list_head	list;

	void			*priv;

	/* FSB of the block we reserved. */
	xfs_fsblock_t		fsbno;

	/* Length of the reservation. */
	xfs_extlen_t		len;

	/* How much of this reservation we've used. */
	xfs_extlen_t		used;
};

struct xrep_newbt {
	struct xfs_scrub	*sc;

	/* List of extents that we've reserved. */
	struct list_head	reservations;

	/* Fake root for new btree. */
	union {
		struct xbtree_afakeroot	afake;
		struct xbtree_ifakeroot	ifake;
	};

	/* rmap owner of these blocks */
	struct xfs_owner_info	oinfo;

	/* The last reservation we allocated from. */
	struct xrep_newbt_resv	*last_resv;

	/* Allocation hint */
	xfs_fsblock_t		alloc_hint;

	/* per-ag reservation type */
	enum xfs_ag_resv_type	resv;
};

#define for_each_xrep_newbt_reservation(xnr, resv, n)	\
	list_for_each_entry_safe((resv), (n), &(xnr)->reservations, list)

void xrep_newbt_init_bare(struct xrep_newbt *xba, struct xfs_scrub *sc);
void xrep_newbt_init_ag(struct xrep_newbt *xba, struct xfs_scrub *sc,
		const struct xfs_owner_info *oinfo, xfs_fsblock_t alloc_hint,
		enum xfs_ag_resv_type resv);
void xrep_newbt_init_inode(struct xrep_newbt *xba, struct xfs_scrub *sc,
		int whichfork, const struct xfs_owner_info *oinfo);
int xrep_newbt_add_reservation(struct xrep_newbt *xba, xfs_fsblock_t fsbno,
		xfs_extlen_t len, void *priv);
int xrep_newbt_reserve_space(struct xrep_newbt *xba, uint64_t nr_blocks);
void xrep_newbt_destroy(struct xrep_newbt *xba, int error);
int xrep_newbt_alloc_block(struct xfs_btree_cur *cur, struct xrep_newbt *xba,
		union xfs_btree_ptr *ptr);
void xrep_bload_estimate_slack(struct xfs_scrub *sc,
		struct xfs_btree_bload *bload);

#else

static inline int xrep_attempt(
	struct xfs_inode	*ip,
	struct xfs_scrub	*sc)
{
	return -EOPNOTSUPP;
}

static inline void xrep_failure(struct xfs_mount *mp) {}

static inline xfs_extlen_t
xrep_calc_ag_resblks(
	struct xfs_scrub	*sc)
{
	ASSERT(!(sc->sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR));
	return 0;
}

static inline int
xrep_reset_perag_resv(
	struct xfs_scrub	*sc)
{
	if (!(sc->flags & XREP_RESET_PERAG_RESV))
		return 0;

	ASSERT(0);
	return -EOPNOTSUPP;
}

#define xrep_revalidate_allocbt		(NULL)
#define xrep_revalidate_iallocbt	(NULL)

#define xrep_probe			xrep_notsupported
#define xrep_superblock			xrep_notsupported
#define xrep_agf			xrep_notsupported
#define xrep_agfl			xrep_notsupported
#define xrep_agi			xrep_notsupported
#define xrep_allocbt			xrep_notsupported
#define xrep_iallocbt			xrep_notsupported

#endif /* CONFIG_XFS_ONLINE_REPAIR */

#endif	/* __XFS_SCRUB_REPAIR_H__ */
