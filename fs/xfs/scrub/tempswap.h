// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_TEMPSWAP_H__
#define __XFS_SCRUB_TEMPSWAP_H__

#ifdef CONFIG_XFS_ONLINE_REPAIR
struct xrep_tempswap {
	struct xfs_swapext_req	req;
};

int xrep_tempswap_grab_log_assist(struct xfs_scrub *sc);
int xrep_tempswap_trans_reserve(struct xfs_scrub *sc, int whichfork,
		struct xrep_tempswap *ti);

int xrep_tempswap_contents(struct xfs_scrub *sc, struct xrep_tempswap *ti);
#endif /* CONFIG_XFS_ONLINE_REPAIR */

#endif /* __XFS_SCRUB_TEMPFILE_H__ */
