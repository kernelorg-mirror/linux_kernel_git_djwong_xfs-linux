/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_EXCHMAPS_H__
#define __XFS_EXCHMAPS_H__

/*
 * Decide if this filesystem has a new enough permanent feature set to protect
 * file mapping exchange log items from being replayed on a kernel that does
 * not have XFS_SB_FEAT_INCOMPAT_LOG_EXCHMAPS set.
 */
static inline bool
xfs_exchmaps_can_use_without_log_assistance(
	struct xfs_mount	*mp)
{
	if (!xfs_sb_is_v5(&mp->m_sb))
		return false;

	/*
	 * No incompat features have been defined since the addition of file
	 * mapping exchange log items.
	 */

	return false;
}

bool xfs_exchmaps_should_advertise(struct xfs_mount *mp);

#endif /* __XFS_EXCHMAPS_H__ */
