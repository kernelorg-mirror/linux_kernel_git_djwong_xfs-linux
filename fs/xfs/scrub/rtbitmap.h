// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_RTBITMAP_H__
#define __XFS_SCRUB_RTBITMAP_H__

struct xchk_rtbitmap {
	uint64_t		rextents;
	uint64_t		rbmblocks;
	unsigned int		rextslog;
	unsigned int		resblks;
};

/*
 * We use an xfile to construct new bitmap blocks for the portion of the
 * rtbitmap file that we're replacing.  Whereas the ondisk bitmap must be
 * accessed through the buffer cache, the xfile bitmap supports direct
 * word-level accesses.  Therefore, we create a small abstraction for linear
 * access.
 */
typedef unsigned long long xrep_wordoff_t;
typedef unsigned int xrep_wordcnt_t;

/* Mask to round an rtx down to the nearest bitmap word. */
#define XREP_RTBMP_WORDMASK	((1ULL << XFS_NBWORDLOG) - 1)

struct xchk_rgbitmap {
	struct xfs_scrub	*sc;

	struct xchk_rtbitmap	rtb;

#ifdef CONFIG_XFS_ONLINE_REPAIR
	struct xfs_rtalloc_args	args;
	struct xrep_tempswap	tempswap;
#endif

	/* The next free rt block that we expect to see. */
	xfs_rtblock_t		next_free_rtblock;

	/* file offset inside the rtbitmap where we start swapping */
	xfs_fileoff_t		group_rbmoff;

	/* number of rtbitmap blocks for this group */
	xfs_filblks_t		group_rbmlen;

	/* The next rtgroup block we expect to see during our rtrmapbt walk. */
	xfs_rgblock_t		next_rgbno;

	/* rtgroup lock flags */
	unsigned int		rtglock_flags;

	/* rtword position of xfile as we write buffers to disk. */
	xrep_wordoff_t		prep_wordoff;

	/* Memory buffer full of 1s for rgbitmap repair. */
	union xfs_rtword_raw	words[];
};

#ifdef CONFIG_XFS_ONLINE_REPAIR
int xrep_setup_rtbitmap(struct xfs_scrub *sc, struct xchk_rtbitmap *rtb);
int xrep_setup_rgbitmap(struct xfs_scrub *sc, struct xchk_rgbitmap *rgb);

/*
 * How big should the words[] buffer be?
 *
 * For repairs, we want a full fsblock worth of space so that we can memcpy a
 * buffer full of 1s into the xfile bitmap.  The xfile bitmap doesn't have
 * rtbitmap block headers, so we don't use blockwsize.  Scrub doesn't use the
 * words buffer at all.
 */
static inline unsigned int
xchk_rgbitmap_wordcnt(
	struct xfs_scrub	*sc)
{
	if (xchk_could_repair(sc))
		return sc->mp->m_sb.sb_blocksize >> XFS_WORDLOG;
	return 0;
}
#else
# define xrep_setup_rtbitmap(sc, rtb)	(0)
# define xrep_setup_rgbitmap(sc, rgb)	(0)
# define xchk_rgbitmap_wordcnt(sc)	(0)
#endif /* CONFIG_XFS_ONLINE_REPAIR */

#endif /* __XFS_SCRUB_RTBITMAP_H__ */
