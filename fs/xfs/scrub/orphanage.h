// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_ORPHANAGE_H__
#define __XFS_SCRUB_ORPHANAGE_H__

#ifdef CONFIG_XFS_ONLINE_REPAIR
int xrep_orphanage_create(struct xfs_scrub *sc);

/*
 * If we're doing a repair, ensure that the orphanage exists and attach it to
 * the scrub context.
 */
static inline int
xrep_orphanage_try_create(
	struct xfs_scrub	*sc)
{
	int			error;

	ASSERT(sc->sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR);

	error = xrep_orphanage_create(sc);
	switch (error) {
	case 0:
	case -ENOENT:
	case -ENOTDIR:
	case -ENOSPC:
		/*
		 * If the orphanage can't be found or isn't a directory, we'll
		 * keep going, but we won't be able to attach the file to the
		 * orphanage if we can't find the parent.
		 */
		return 0;
	}

	return error;
}

int xrep_orphanage_iolock_two(struct xfs_scrub *sc);

/* Information about a request to add a file to the orphanage. */
struct xrep_orphanage_req {
	/* Name structure; caller must provide a buffer separately. */
	struct xfs_name		xname;

	struct xfs_scrub	*sc;

	/* Block reservations for orphanage and child (if directory). */
	unsigned int		orphanage_blkres;
	unsigned int		child_blkres;
};

static inline size_t
xrep_orphanage_req_sizeof(void)
{
	return sizeof(struct xrep_orphanage_req) + MAXNAMELEN + 1;
}

void xrep_orphanage_compute_blkres(struct xfs_scrub *sc,
		struct xrep_orphanage_req *orph);
int xrep_orphanage_compute_name(struct xrep_orphanage_req *orph,
		unsigned char *namebuf);
int xrep_orphanage_ilock_resv_quota(struct xrep_orphanage_req *orph);
int xrep_orphanage_adopt(struct xrep_orphanage_req *orph);

void xrep_orphanage_ilock(struct xfs_scrub *sc, unsigned int ilock_flags);
bool xrep_orphanage_ilock_nowait(struct xfs_scrub *sc,
		unsigned int ilock_flags);
void xrep_orphanage_iunlock(struct xfs_scrub *sc, unsigned int ilock_flags);

void xrep_orphanage_rele(struct xfs_scrub *sc);
#else
# define xrep_orphanage_rele(sc)
#endif /* CONFIG_XFS_ONLINE_REPAIR */

#endif /* __XFS_SCRUB_ORPHANAGE_H__ */
