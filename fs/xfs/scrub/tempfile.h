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
void xrep_tempfile_ilock_two(struct xfs_scrub *sc, unsigned int ilock_flags);

int xrep_tempfile_prealloc(struct xfs_scrub *sc, xfs_fileoff_t off,
		xfs_filblks_t len);

enum xfs_blft;

int xrep_tempfile_copyin_xfile(struct xfs_scrub *sc,
		const struct xfs_buf_ops *ops, enum xfs_blft type,
		xfs_fileoff_t isize);

void xrep_tempfile_copyout_local(struct xfs_scrub *sc, int whichfork);

struct xfs_swapext_req;
struct xfs_swapext_res;
int xrep_tempfile_swapext_prep_request(struct xfs_scrub *sc, int whichfork,
		struct xfs_swapext_req *req);
int xrep_tempfile_swapext_prep(struct xfs_scrub *sc, int whichfork,
		struct xfs_swapext_req *req, struct xfs_swapext_res *res);
int xrep_tempfile_swapext_trans_alloc(struct xfs_scrub *sc,
		struct xfs_swapext_res *res);
int xrep_tempfile_swapext(struct xfs_scrub *sc, struct xfs_swapext_req *req);
#else
# define xrep_tempfile_rele(sc)
#endif /* CONFIG_XFS_ONLINE_REPAIR */

#endif /* __XFS_SCRUB_TEMPFILE_H__ */
