// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_RTSUMMARY_H__
#define __XFS_SCRUB_RTSUMMARY_H__

typedef unsigned int xchk_rtsumoff_t;

int xfsum_copyout(struct xfs_scrub *sc, xchk_rtsumoff_t sumoff,
		xfs_suminfo_t *info, unsigned int nr_words);

#endif /* __XFS_SCRUB_RTSUMMARY_H__ */
