// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SCRUB_REPAIR_H__
#define __XFS_SCRUB_REPAIR_H__

#include "xfs_quota_defs.h"

static inline int xrep_notsupported(struct xfs_scrub *sc)
{
	return -EOPNOTSUPP;
}

#ifdef CONFIG_XFS_ONLINE_REPAIR

/* Repair helpers */

int xrep_attempt(struct xfs_scrub *sc);
bool xrep_will_attempt(struct xfs_scrub *sc);
void xrep_failure(struct xfs_mount *mp);
int xrep_roll_ag_trans(struct xfs_scrub *sc);
int xrep_roll_trans(struct xfs_scrub *sc);
int xrep_defer_finish(struct xfs_scrub *sc);
bool xrep_ag_has_space(struct xfs_perag *pag, xfs_extlen_t nr_blocks,
		enum xfs_ag_resv_type type);
xfs_extlen_t xrep_calc_ag_resblks(struct xfs_scrub *sc);

static inline int
xrep_trans_commit(
	struct xfs_scrub	*sc)
{
	int			error = xfs_trans_commit(sc->tp);

	sc->tp = NULL;
	return error;
}

struct xbitmap;

int xrep_fix_freelist(struct xfs_scrub *sc, bool can_shrink);

struct xrep_find_ag_btree {
	/* in: rmap owner of the btree we're looking for */
	uint64_t			rmap_owner;

	/* in: buffer ops */
	const struct xfs_buf_ops	*buf_ops;

	/* in: maximum btree height */
	unsigned int			maxlevels;

	/* out: the highest btree block found and the tree height */
	xfs_agblock_t			root;
	unsigned int			height;
};

int xrep_find_ag_btree_roots(struct xfs_scrub *sc, struct xfs_buf *agf_bp,
		struct xrep_find_ag_btree *btree_info, struct xfs_buf *agfl_bp);
void xrep_update_qflags(struct xfs_scrub *sc, unsigned int clear_flags,
		unsigned int set_flags);
void xrep_force_quotacheck(struct xfs_scrub *sc, xfs_dqtype_t type);
int xrep_ino_dqattach(struct xfs_scrub *sc);
int xrep_ino_ensure_extent_count(struct xfs_scrub *sc, int whichfork,
		xfs_extnum_t nextents);
int xrep_reset_perag_resv(struct xfs_scrub *sc);
int xrep_bmap(struct xfs_scrub *sc, int whichfork, bool allow_unwritten);
int xrep_metadata_inode_forks(struct xfs_scrub *sc);

/* Repair setup functions */
int xrep_setup_ag_allocbt(struct xfs_scrub *sc);

struct xfs_imap;
int xrep_setup_inode(struct xfs_scrub *sc, struct xfs_imap *imap);
int xrep_setup_rtbitmap(struct xfs_scrub *sc, unsigned int *resblks);

void xrep_ag_btcur_init(struct xfs_scrub *sc, struct xchk_ag *sa);
int xrep_ag_init(struct xfs_scrub *sc, struct xfs_perag *pag,
		struct xchk_ag *sa);

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
int xrep_refcountbt(struct xfs_scrub *sc);
int xrep_inode(struct xfs_scrub *sc);
int xrep_bmap_data(struct xfs_scrub *sc);
int xrep_bmap_attr(struct xfs_scrub *sc);
int xrep_bmap_cow(struct xfs_scrub *sc);
int xrep_nlinks(struct xfs_scrub *sc);
int xrep_fscounters(struct xfs_scrub *sc);

#ifdef CONFIG_XFS_RT
int xrep_rtbitmap(struct xfs_scrub *sc);
#else
# define xrep_rtbitmap			xrep_notsupported
#endif /* CONFIG_XFS_RT */

#ifdef CONFIG_XFS_QUOTA
int xrep_quota(struct xfs_scrub *sc);
int xrep_quotacheck(struct xfs_scrub *sc);
#else
# define xrep_quota			xrep_notsupported
# define xrep_quotacheck		xrep_notsupported
#endif /* CONFIG_XFS_QUOTA */

int xrep_reinit_pagf(struct xfs_scrub *sc);
int xrep_reinit_pagi(struct xfs_scrub *sc);

#else

#define xrep_will_attempt(sc)	(false)

static inline int
xrep_attempt(
	struct xfs_scrub	*sc)
{
	return -EOPNOTSUPP;
}

static inline void xrep_failure(struct xfs_mount *mp) {}

static inline xfs_extlen_t
xrep_calc_ag_resblks(
	struct xfs_scrub	*sc)
{
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

/* repair setup functions for no-repair */
static inline int
xrep_setup_nothing(
	struct xfs_scrub	*sc)
{
	return 0;
}
#define xrep_setup_ag_allocbt		xrep_setup_nothing

#define xrep_setup_inode(sc, imap)	((void)0)

static inline int xrep_setup_rtbitmap(struct xfs_scrub *sc, unsigned int *x)
{
	return 0;
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
#define xrep_refcountbt			xrep_notsupported
#define xrep_inode			xrep_notsupported
#define xrep_bmap_data			xrep_notsupported
#define xrep_bmap_attr			xrep_notsupported
#define xrep_bmap_cow			xrep_notsupported
#define xrep_rtbitmap			xrep_notsupported
#define xrep_quota			xrep_notsupported
#define xrep_quotacheck			xrep_notsupported
#define xrep_nlinks			xrep_notsupported
#define xrep_fscounters			xrep_notsupported

#endif /* CONFIG_XFS_ONLINE_REPAIR */

#endif	/* __XFS_SCRUB_REPAIR_H__ */
