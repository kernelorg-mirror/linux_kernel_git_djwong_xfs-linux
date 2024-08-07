/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2022-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __LIBXFS_RTGROUP_H
#define __LIBXFS_RTGROUP_H 1

struct xfs_mount;
struct xfs_trans;

/*
 * Realtime group incore structure, similar to the per-AG structure.
 */
struct xfs_rtgroup {
	struct xfs_mount	*rtg_mount;
	xfs_rgnumber_t		rtg_rgno;
	atomic_t		rtg_ref;	/* passive reference count */
	atomic_t		rtg_active_ref;	/* active reference count */
	wait_queue_head_t	rtg_active_wq;/* woken active_ref falls to zero */

	/* Number of blocks in this group */
	xfs_rtxnum_t		rtg_extents;

#ifdef __KERNEL__
	/* -- kernel only structures below this line -- */
	spinlock_t		rtg_state_lock;
#endif /* __KERNEL__ */
};

#ifdef CONFIG_XFS_RT
/* Passive rtgroup references */
struct xfs_rtgroup *xfs_rtgroup_get(struct xfs_mount *mp, xfs_rgnumber_t rgno);
struct xfs_rtgroup *xfs_rtgroup_hold(struct xfs_rtgroup *rtg);
void xfs_rtgroup_put(struct xfs_rtgroup *rtg);

/* Active rtgroup references */
struct xfs_rtgroup *xfs_rtgroup_grab(struct xfs_mount *mp, xfs_rgnumber_t rgno);
void xfs_rtgroup_rele(struct xfs_rtgroup *rtg);

int xfs_rtgroup_alloc(struct xfs_mount *mp, xfs_rgnumber_t rgno);
void xfs_rtgroup_free(struct xfs_mount *mp, xfs_rgnumber_t rgno);
void xfs_free_rtgroups(struct xfs_mount *mp, xfs_rgnumber_t rgcount);
#else /* CONFIG_XFS_RT */
static inline struct xfs_rtgroup *xfs_rtgroup_get(struct xfs_mount *mp,
		xfs_rgnumber_t rgno)
{
	return NULL;
}
static inline struct xfs_rtgroup *xfs_rtgroup_hold(struct xfs_rtgroup *rtg)
{
	ASSERT(rtg == NULL);
	return NULL;
}
static inline void xfs_rtgroup_put(struct xfs_rtgroup *rtg)
{
}
static inline int xfs_rtgroup_alloc( struct xfs_mount *mp,
		xfs_rgnumber_t rgno)
{
	return 0;
}
static inline void xfs_free_rtgroups(struct xfs_mount *mp,
		xfs_rgnumber_t rgcount)
{
}
#define xfs_rtgroup_grab			xfs_rtgroup_get
#define xfs_rtgroup_rele			xfs_rtgroup_put
#endif /* CONFIG_XFS_RT */

/*
 * rt group iteration APIs
 */
static inline struct xfs_rtgroup *
xfs_rtgroup_next(
	struct xfs_rtgroup	*rtg,
	xfs_rgnumber_t		*rgno,
	xfs_rgnumber_t		end_rgno)
{
	struct xfs_mount	*mp = rtg->rtg_mount;

	*rgno = rtg->rtg_rgno + 1;
	xfs_rtgroup_rele(rtg);
	if (*rgno > end_rgno)
		return NULL;
	return xfs_rtgroup_grab(mp, *rgno);
}

#define for_each_rtgroup_range(mp, rgno, end_rgno, rtg) \
	for ((rtg) = xfs_rtgroup_grab((mp), (rgno)); \
		(rtg) != NULL; \
		(rtg) = xfs_rtgroup_next((rtg), &(rgno), (end_rgno)))

#define for_each_rtgroup_from(mp, rgno, rtg) \
	for_each_rtgroup_range((mp), (rgno), (mp)->m_sb.sb_rgcount - 1, (rtg))


#define for_each_rtgroup(mp, rgno, rtg) \
	(rgno) = 0; \
	for_each_rtgroup_from((mp), (rgno), (rtg))

static inline bool
xfs_verify_rgbno(
	struct xfs_rtgroup	*rtg,
	xfs_rgblock_t		rgbno)
{
	struct xfs_mount	*mp = rtg->rtg_mount;

	if (rgbno >= rtg->rtg_extents * mp->m_sb.sb_rextsize)
		return false;
	if (xfs_has_rtsb(mp) && rtg->rtg_rgno == 0 &&
	    rgbno < mp->m_sb.sb_rextsize)
		return false;
	return true;
}

static inline bool
xfs_verify_rgbext(
	struct xfs_rtgroup	*rtg,
	xfs_rgblock_t		rgbno,
	xfs_rgblock_t		len)
{
	if (rgbno + len <= rgbno)
		return false;

	if (!xfs_verify_rgbno(rtg, rgbno))
		return false;

	return xfs_verify_rgbno(rtg, rgbno + len - 1);
}

static inline xfs_rtblock_t
xfs_rgno_start_rtb(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	if (mp->m_rgblklog >= 0)
		return ((xfs_rtblock_t)rgno << mp->m_rgblklog);
	return ((xfs_rtblock_t)rgno * mp->m_rgblocks);
}

static inline xfs_rtblock_t
xfs_rgbno_to_rtb(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno,
	xfs_rgblock_t		rgbno)
{
	return xfs_rgno_start_rtb(mp, rgno) + rgbno;
}

static inline xfs_rgnumber_t
xfs_rtb_to_rgno(
	struct xfs_mount	*mp,
	xfs_rtblock_t		rtbno)
{
	if (!xfs_has_rtgroups(mp))
		return 0;

	if (mp->m_rgblklog >= 0)
		return rtbno >> mp->m_rgblklog;

	return div_u64(rtbno, mp->m_rgblocks);
}

static inline uint64_t
__xfs_rtb_to_rgbno(
	struct xfs_mount	*mp,
	xfs_rtblock_t		rtbno)
{
	uint32_t		rem;

	if (!xfs_has_rtgroups(mp))
		return rtbno;

	if (mp->m_rgblklog >= 0)
		return rtbno & mp->m_rgblkmask;

	div_u64_rem(rtbno, mp->m_rgblocks, &rem);
	return rem;
}

static inline xfs_rgblock_t
xfs_rtb_to_rgbno(
	struct xfs_mount	*mp,
	xfs_rtblock_t		rtbno)
{
	return __xfs_rtb_to_rgbno(mp, rtbno);
}

static inline xfs_daddr_t
xfs_rtb_to_daddr(
	struct xfs_mount	*mp,
	xfs_rtblock_t		rtbno)
{
	return rtbno << mp->m_blkbb_log;
}

static inline xfs_rtblock_t
xfs_daddr_to_rtb(
	struct xfs_mount	*mp,
	xfs_daddr_t		daddr)
{
	return daddr >> mp->m_blkbb_log;
}

#ifdef CONFIG_XFS_RT
xfs_rtxnum_t xfs_rtgroup_extents(struct xfs_mount *mp, xfs_rgnumber_t rgno);

/* Lock the rt bitmap inode in exclusive mode */
#define XFS_RTGLOCK_BITMAP		(1U << 0)
/* Lock the rt bitmap inode in shared mode */
#define XFS_RTGLOCK_BITMAP_SHARED	(1U << 1)

#define XFS_RTGLOCK_ALL_FLAGS	(XFS_RTGLOCK_BITMAP | \
				 XFS_RTGLOCK_BITMAP_SHARED)

void xfs_rtgroup_lock(struct xfs_rtgroup *rtg, unsigned int rtglock_flags);
void xfs_rtgroup_unlock(struct xfs_rtgroup *rtg, unsigned int rtglock_flags);
void xfs_rtgroup_trans_join(struct xfs_trans *tp, struct xfs_rtgroup *rtg,
		unsigned int rtglock_flags);
#else
# define xfs_rtgroup_extents(mp, rgno)		(0)
# define xfs_rtgroup_lock(rtg, gf)		((void)0)
# define xfs_rtgroup_unlock(rtg, gf)		((void)0)
# define xfs_rtgroup_trans_join(tp, rtg, gf)	((void)0)
#endif /* CONFIG_XFS_RT */

#endif /* __LIBXFS_RTGROUP_H */
