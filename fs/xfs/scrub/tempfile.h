// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_TEMPFILE_H__
#define __XFS_SCRUB_TEMPFILE_H__

#ifdef CONFIG_XFS_ONLINE_REPAIR
int xrep_tempfile_create(struct xfs_scrub *sc, uint16_t mode);
void xrep_tempfile_rele(struct xfs_scrub *sc);

void xrep_tempfile_ilock(struct xfs_scrub *sc, unsigned int ilock_flags);
bool xrep_tempfile_ilock_nowait(struct xfs_scrub *sc, unsigned int ilock_flags);
void xrep_tempfile_iunlock(struct xfs_scrub *sc, unsigned int ilock_flags);
#else
# define xrep_tempfile_rele(sc)
#endif /* CONFIG_XFS_ONLINE_REPAIR */

#endif /* __XFS_SCRUB_TEMPFILE_H__ */
