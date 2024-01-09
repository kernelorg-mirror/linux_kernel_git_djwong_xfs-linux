// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_exchmaps.h"

/*
 * Decide if we should advertise to userspace the potential for using file
 * mapping exchanges on this filesystem.  This does not say anything about the
 * actual readiness to start such an operation.
 */
bool
xfs_exchmaps_should_advertise(
	struct xfs_mount	*mp)
{
	/*
	 * If the filesystem doesn't need XFS_SB_FEAT_INCOMPAT_LOG_EXCHMAPS to
	 * protect mapping exchange log items, then we don't have to deal with
	 * old kernels or upgraded filesystems.  Advertise the capability.
	 */
	if (xfs_exchmaps_can_use_without_log_assistance(mp))
		return true;

	/*
	 * If the filesystem has relatively new features enabled, we're willing
	 * to upgrade the filesystem to have the EXCHMAPS log incompat feature.
	 * Technically we could do this with any V5 filesystem, but let's not
	 * deal with really old kernels.
	 */
	if (xfs_has_bigtime(mp) || xfs_has_large_extent_counts(mp))
		return true;

	return false;
}
