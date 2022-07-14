/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SWAPEXT_H_
#define __XFS_SWAPEXT_H_ 1

/*
 * Decide if this filesystem supports using log items to swap file extents and
 * restart the operation if the system fails before the operation completes.
 *
 * This can be done to individual file extents by using the block mapping log
 * intent items introduced with reflink and rmap; or to entire file ranges
 * using swapext log intent items to track the overall progress across multiple
 * extent mappings.  Realtime is not supported yet.
 */
static inline bool xfs_swapext_supported(struct xfs_mount *mp)
{
	return (xfs_has_reflink(mp) || xfs_has_rmapbt(mp)) &&
	       !xfs_has_realtime(mp);
}

#endif /* __XFS_SWAPEXT_H_ */
