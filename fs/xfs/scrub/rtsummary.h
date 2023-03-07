// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_RTSUMMARY_H__
#define __XFS_SCRUB_RTSUMMARY_H__

struct xchk_rtsummary {
#ifdef CONFIG_XFS_ONLINE_REPAIR
	struct xrep_tempswap	tempswap;
#endif

	uint64_t		rextents;
	uint64_t		rbmblocks;
	uint64_t		rsumsize;
	unsigned int		rsumlevels;
	unsigned int		resblks;

	/* suminfo position of xfile as we write buffers to disk. */
	xfs_rtsumoff_t		prep_wordoff;

	/* Memory buffer for the summary comparison. */
	xfs_suminfo_t		words[];
};

int xfsum_copyout(struct xfs_scrub *sc, xfs_rtsumoff_t sumoff,
		xfs_suminfo_t *info, unsigned int nr_words);

#ifdef CONFIG_XFS_ONLINE_REPAIR
int xrep_setup_rtsummary(struct xfs_scrub *sc, struct xchk_rtsummary *rts);
#else
# define xrep_setup_rtsummary(sc, rts)	(0)
#endif /* CONFIG_XFS_ONLINE_REPAIR */

#endif /* __XFS_SCRUB_RTSUMMARY_H__ */
