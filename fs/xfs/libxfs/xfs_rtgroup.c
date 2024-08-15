// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_rmap_btree.h"
#include "xfs_alloc.h"
#include "xfs_ialloc.h"
#include "xfs_rmap.h"
#include "xfs_ag.h"
#include "xfs_ag_resv.h"
#include "xfs_health.h"
#include "xfs_error.h"
#include "xfs_bmap.h"
#include "xfs_defer.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_trace.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_buf_item.h"
#include "xfs_rtgroup.h"
#include "xfs_rtbitmap.h"
#include "xfs_metafile.h"
#include "xfs_metadir.h"

/*
 * Passive reference counting access wrappers to the rtgroup structures.  If
 * the rtgroup structure is to be freed, the freeing code is responsible for
 * cleaning up objects with passive references before freeing the structure.
 */
struct xfs_rtgroup *
xfs_rtgroup_get(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	struct xfs_rtgroup	*rtg;

	rcu_read_lock();
	rtg = xa_load(&mp->m_rtgroups, rgno);
	if (rtg) {
		trace_xfs_rtgroup_get(rtg, _RET_IP_);
		ASSERT(atomic_read(&rtg->rtg_ref) >= 0);
		atomic_inc(&rtg->rtg_ref);
	}
	rcu_read_unlock();
	return rtg;
}

/* Get a passive reference to the given rtgroup. */
struct xfs_rtgroup *
xfs_rtgroup_hold(
	struct xfs_rtgroup	*rtg)
{
	ASSERT(atomic_read(&rtg->rtg_ref) > 0 ||
	       atomic_read(&rtg->rtg_active_ref) > 0);

	trace_xfs_rtgroup_hold(rtg, _RET_IP_);
	atomic_inc(&rtg->rtg_ref);
	return rtg;
}

void
xfs_rtgroup_put(
	struct xfs_rtgroup	*rtg)
{
	trace_xfs_rtgroup_put(rtg, _RET_IP_);
	ASSERT(atomic_read(&rtg->rtg_ref) > 0);
	atomic_dec(&rtg->rtg_ref);
}

/*
 * Active references for rtgroup structures. This is for short term access to
 * the rtgroup structures for walking trees or accessing state. If an rtgroup
 * is being shrunk or is offline, then this will fail to find that group and
 * return NULL instead.
 */
struct xfs_rtgroup *
xfs_rtgroup_grab(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno)
{
	struct xfs_rtgroup	*rtg;

	rcu_read_lock();
	rtg = xa_load(&mp->m_rtgroups, agno);
	if (rtg) {
		trace_xfs_rtgroup_grab(rtg, _RET_IP_);
		if (!atomic_inc_not_zero(&rtg->rtg_active_ref))
			rtg = NULL;
	}
	rcu_read_unlock();
	return rtg;
}

void
xfs_rtgroup_rele(
	struct xfs_rtgroup	*rtg)
{
	trace_xfs_rtgroup_rele(rtg, _RET_IP_);
	if (atomic_dec_and_test(&rtg->rtg_active_ref))
		wake_up(&rtg->rtg_active_wq);
}

int
xfs_rtgroup_alloc(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	struct xfs_rtgroup	*rtg;
	int			error;

	rtg = kzalloc(sizeof(struct xfs_rtgroup), GFP_KERNEL);
	if (!rtg)
		return -ENOMEM;
	rtg->rtg_rgno = rgno;
	rtg->rtg_mount = mp;

	error = xa_insert(&mp->m_rtgroups, rgno, rtg, GFP_KERNEL);
	if (error) {
		WARN_ON_ONCE(error == -EBUSY);
		goto out_free_rtg;
	}

#ifdef __KERNEL__
	/* Place kernel structure only init below this point. */
	spin_lock_init(&rtg->rtg_state_lock);
	init_waitqueue_head(&rtg->rtg_active_wq);
#endif /* __KERNEL__ */

	/* Active ref owned by mount indicates rtgroup is online. */
	atomic_set(&rtg->rtg_active_ref, 1);
	return 0;

out_free_rtg:
	kfree(rtg);
	return error;
}

void
xfs_rtgroup_free(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	struct xfs_rtgroup	*rtg;

	rtg = xa_erase(&mp->m_rtgroups, rgno);
	if (!rtg) /* can happen when growfs fails */
		return;

	XFS_IS_CORRUPT(mp, atomic_read(&rtg->rtg_ref) != 0);

	/* drop the mount's active reference */
	xfs_rtgroup_rele(rtg);
	XFS_IS_CORRUPT(mp, atomic_read(&rtg->rtg_active_ref) != 0);

	kfree_rcu_mightsleep(rtg);
}

/*
 * Free up the rtgroup resources associated with the mount structure.
 */
void
xfs_free_rtgroups(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgcount)
{
	xfs_rgnumber_t		rgno;

	for (rgno = 0; rgno < rgcount; rgno++)
		xfs_rtgroup_free(mp, rgno);
}

/* Compute the number of rt extents in this realtime group. */
xfs_rtxnum_t
xfs_rtgroup_extents(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	xfs_rgnumber_t		rgcount = mp->m_sb.sb_rgcount;

	ASSERT(rgno < rgcount);
	if (rgno == rgcount - 1)
		return mp->m_sb.sb_rextents -
			((xfs_rtxnum_t)rgno * mp->m_sb.sb_rgextents);

	ASSERT(xfs_has_rtgroups(mp));
	return mp->m_sb.sb_rgextents;
}

/* Lock metadata inodes associated with this rt group. */
void
xfs_rtgroup_lock(
	struct xfs_rtgroup	*rtg,
	unsigned int		rtglock_flags)
{
	ASSERT(!(rtglock_flags & ~XFS_RTGLOCK_ALL_FLAGS));
	ASSERT(!(rtglock_flags & XFS_RTGLOCK_BITMAP_SHARED) ||
	       !(rtglock_flags & XFS_RTGLOCK_BITMAP));

	if (rtglock_flags & XFS_RTGLOCK_BITMAP) {
		/*
		 * Lock both realtime free space metadata inodes for a freespace
		 * update.
		 */
		xfs_ilock(rtg->rtg_inodes[XFS_RTGI_BITMAP], XFS_ILOCK_EXCL);
		xfs_ilock(rtg->rtg_inodes[XFS_RTGI_SUMMARY], XFS_ILOCK_EXCL);
	} else if (rtglock_flags & XFS_RTGLOCK_BITMAP_SHARED) {
		xfs_ilock(rtg->rtg_inodes[XFS_RTGI_BITMAP], XFS_ILOCK_SHARED);
	}
}

/* Unlock metadata inodes associated with this rt group. */
void
xfs_rtgroup_unlock(
	struct xfs_rtgroup	*rtg,
	unsigned int		rtglock_flags)
{
	ASSERT(!(rtglock_flags & ~XFS_RTGLOCK_ALL_FLAGS));
	ASSERT(!(rtglock_flags & XFS_RTGLOCK_BITMAP_SHARED) ||
	       !(rtglock_flags & XFS_RTGLOCK_BITMAP));

	if (rtglock_flags & XFS_RTGLOCK_BITMAP) {
		xfs_iunlock(rtg->rtg_inodes[XFS_RTGI_SUMMARY], XFS_ILOCK_EXCL);
		xfs_iunlock(rtg->rtg_inodes[XFS_RTGI_BITMAP], XFS_ILOCK_EXCL);
	} else if (rtglock_flags & XFS_RTGLOCK_BITMAP_SHARED) {
		xfs_iunlock(rtg->rtg_inodes[XFS_RTGI_BITMAP], XFS_ILOCK_SHARED);
	}
}

/*
 * Join realtime group metadata inodes to the transaction.  The ILOCKs will be
 * released on transaction commit.
 */
void
xfs_rtgroup_trans_join(
	struct xfs_trans	*tp,
	struct xfs_rtgroup	*rtg,
	unsigned int		rtglock_flags)
{
	ASSERT(!(rtglock_flags & ~XFS_RTGLOCK_ALL_FLAGS));
	ASSERT(!(rtglock_flags & XFS_RTGLOCK_BITMAP_SHARED));

	if (rtglock_flags & XFS_RTGLOCK_BITMAP) {
		xfs_trans_ijoin(tp, rtg->rtg_inodes[XFS_RTGI_BITMAP],
				XFS_ILOCK_EXCL);
		xfs_trans_ijoin(tp, rtg->rtg_inodes[XFS_RTGI_SUMMARY],
				XFS_ILOCK_EXCL);
	}
}

/* Retrieve rt group geometry. */
int
xfs_rtgroup_get_geometry(
	struct xfs_rtgroup	*rtg,
	struct xfs_rtgroup_geometry *rgeo)
{
	/* Fill out form. */
	memset(rgeo, 0, sizeof(*rgeo));
	rgeo->rg_number = rtg->rtg_rgno;
	rgeo->rg_length = rtg->rtg_extents * rtg->rtg_mount->m_sb.sb_rextsize;
	rgeo->rg_capacity = rgeo->rg_length;
	xfs_rtgroup_geom_health(rtg, rgeo);
	return 0;
}

#ifdef CONFIG_PROVE_LOCKING
static struct lock_class_key xfs_rtginode_lock_class;

static int
xfs_rtginode_ilock_cmp_fn(
	const struct lockdep_map	*m1,
	const struct lockdep_map	*m2)
{
	const struct xfs_inode *ip1 =
		container_of(m1, struct xfs_inode, i_lock.dep_map);
	const struct xfs_inode *ip2 =
		container_of(m2, struct xfs_inode, i_lock.dep_map);

	if (ip1->i_projid < ip2->i_projid)
		return -1;
	if (ip1->i_projid > ip2->i_projid)
		return 1;
	return 0;
}

static inline void
xfs_rtginode_ilock_print_fn(
	const struct lockdep_map	*m)
{
	const struct xfs_inode *ip =
		container_of(m, struct xfs_inode, i_lock.dep_map);

	printk(KERN_CONT " rgno=%u", ip->i_projid);
}

/*
 * Most of the time each of the RTG inode locks are only taken one at a time.
 * But when committing deferred ops, more than one of a kind can be taken.
 * However, deferred rt ops will be committed in rgno order so there is no
 * potential for deadlocks.  The code here is needed to tell lockdep about this
 * order.
 */
static inline void
xfs_rtginode_lockdep_setup(
	struct xfs_inode	*ip,
	xfs_rgnumber_t		rgno,
	enum xfs_rtg_inodes	type)
{
	lockdep_set_class_and_subclass(&ip->i_lock, &xfs_rtginode_lock_class,
			type);
	lock_set_cmp_fn(&ip->i_lock, xfs_rtginode_ilock_cmp_fn,
			xfs_rtginode_ilock_print_fn);
}
#else
#define xfs_rtginode_lockdep_setup(ip, rgno, type)	do { } while (0)
#endif /* CONFIG_PROVE_LOCKING */

struct xfs_rtginode_ops {
	const char		*name;	/* short name */

	enum xfs_metafile_type	metafile_type;

	unsigned int		sick;	/* rtgroup sickness flag */

	/* Does the fs have this feature? */
	bool			(*enabled)(struct xfs_mount *mp);

	/* Create this rtgroup metadata inode and initialize it. */
	int			(*create)(struct xfs_rtgroup *rtg,
					  struct xfs_inode *ip,
					  struct xfs_trans *tp,
					  bool init);
};

static const struct xfs_rtginode_ops xfs_rtginode_ops[XFS_RTGI_MAX] = {
	[XFS_RTGI_BITMAP] = {
		.name		= "bitmap",
		.metafile_type	= XFS_METAFILE_RTBITMAP,
		.sick		= XFS_SICK_RG_BITMAP,
		.create		= xfs_rtbitmap_create,
	},
	[XFS_RTGI_SUMMARY] = {
		.name		= "summary",
		.metafile_type	= XFS_METAFILE_RTSUMMARY,
		.sick		= XFS_SICK_RG_SUMMARY,
		.create		= xfs_rtsummary_create,
	},
};

/* Return the shortname of this rtgroup inode. */
const char *
xfs_rtginode_name(
	enum xfs_rtg_inodes	type)
{
	return xfs_rtginode_ops[type].name;
}

/* Return the metafile type of this rtgroup inode. */
enum xfs_metafile_type
xfs_rtginode_metafile_type(
	enum xfs_rtg_inodes	type)
{
	return xfs_rtginode_ops[type].metafile_type;
}

/* Should this rtgroup inode be present? */
bool
xfs_rtginode_enabled(
	struct xfs_rtgroup	*rtg,
	enum xfs_rtg_inodes	type)
{
	const struct xfs_rtginode_ops *ops = &xfs_rtginode_ops[type];

	if (!ops->enabled)
		return true;
	return ops->enabled(rtg->rtg_mount);
}

/* Mark an rtgroup inode sick */
void
xfs_rtginode_mark_sick(
	struct xfs_rtgroup	*rtg,
	enum xfs_rtg_inodes	type)
{
	const struct xfs_rtginode_ops *ops = &xfs_rtginode_ops[type];

	xfs_rtgroup_mark_sick(rtg, ops->sick);
}

/* Load and existing rtgroup inode into the rtgroup structure. */
int
xfs_rtginode_load(
	struct xfs_rtgroup	*rtg,
	enum xfs_rtg_inodes	type,
	struct xfs_trans	*tp)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_inode	*ip;
	const struct xfs_rtginode_ops *ops = &xfs_rtginode_ops[type];
	int			error;

	if (!xfs_rtginode_enabled(rtg, type))
		return 0;

	if (!xfs_has_rtgroups(mp)) {
		xfs_ino_t	ino;

		switch (type) {
		case XFS_RTGI_BITMAP:
			ino = mp->m_sb.sb_rbmino;
			break;
		case XFS_RTGI_SUMMARY:
			ino = mp->m_sb.sb_rsumino;
			break;
		default:
			/* None of the other types exist on !rtgroups */
			return 0;
		}

		error = xfs_trans_metafile_iget(tp, ino, ops->metafile_type,
				&ip);
	} else {
		const char	*path;

		if (!mp->m_rtdirip) {
			xfs_fs_mark_sick(mp, XFS_SICK_FS_METADIR);
			return -EFSCORRUPTED;
		}

		path = xfs_rtginode_path(rtg->rtg_rgno, type);
		if (!path)
			return -ENOMEM;
		error = xfs_metadir_load(tp, mp->m_rtdirip, path,
				ops->metafile_type, &ip);
		kfree(path);
	}

	if (error) {
		if (xfs_metadata_is_sick(error))
			xfs_rtginode_mark_sick(rtg, type);
		return error;
	}

	if (XFS_IS_CORRUPT(mp, ip->i_df.if_format != XFS_DINODE_FMT_EXTENTS &&
			       ip->i_df.if_format != XFS_DINODE_FMT_BTREE)) {
		xfs_irele(ip);
		xfs_rtginode_mark_sick(rtg, type);
		return -EFSCORRUPTED;
	}

	if (XFS_IS_CORRUPT(mp, ip->i_projid != rtg->rtg_rgno)) {
		xfs_irele(ip);
		xfs_rtginode_mark_sick(rtg, type);
		return -EFSCORRUPTED;
	}

	xfs_rtginode_lockdep_setup(ip, rtg->rtg_rgno, type);
	rtg->rtg_inodes[type] = ip;
	return 0;
}

/* Release an rtgroup metadata inode. */
void
xfs_rtginode_irele(
	struct xfs_inode	**ipp)
{
	if (*ipp)
		xfs_irele(*ipp);
	*ipp = NULL;
}

/* Add a metadata inode for a realtime rmap btree. */
int
xfs_rtginode_create(
	struct xfs_rtgroup		*rtg,
	enum xfs_rtg_inodes		type,
	bool				init)
{
	const struct xfs_rtginode_ops	*ops = &xfs_rtginode_ops[type];
	struct xfs_mount		*mp = rtg->rtg_mount;
	struct xfs_metadir_update	upd = {
		.dp			= mp->m_rtdirip,
		.metafile_type		= ops->metafile_type,
	};
	int				error;

	if (!xfs_rtginode_enabled(rtg, type))
		return 0;

	if (!mp->m_rtdirip) {
		xfs_fs_mark_sick(mp, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

	upd.path = xfs_rtginode_path(rtg->rtg_rgno, type);
	if (!upd.path)
		return -ENOMEM;

	error = xfs_metadir_start_create(&upd);
	if (error)
		goto out_path;

	error = xfs_metadir_create(&upd, S_IFREG);
	if (error)
		return error;

	xfs_rtginode_lockdep_setup(upd.ip, rtg->rtg_rgno, type);

	upd.ip->i_projid = rtg->rtg_rgno;
	error = ops->create(rtg, upd.ip, upd.tp, init);
	if (error)
		goto out_cancel;

	error = xfs_metadir_commit(&upd);
	if (error)
		goto out_path;

	kfree(upd.path);
	xfs_finish_inode_setup(upd.ip);
	rtg->rtg_inodes[type] = upd.ip;
	return 0;

out_cancel:
	xfs_metadir_cancel(&upd, error);
	/* Have to finish setting up the inode to ensure it's deleted. */
	if (upd.ip) {
		xfs_finish_inode_setup(upd.ip);
		xfs_irele(upd.ip);
	}
out_path:
	kfree(upd.path);
	return error;
}

/* Create the parent directory for all rtgroup inodes and load it. */
int
xfs_rtginode_mkdir_parent(
	struct xfs_mount	*mp)
{
	if (!mp->m_metadirip) {
		xfs_fs_mark_sick(mp, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

	return xfs_metadir_mkdir(mp->m_metadirip, "rtgroups", &mp->m_rtdirip);
}

/* Load the parent directory of all rtgroup inodes. */
int
xfs_rtginode_load_parent(
	struct xfs_trans	*tp)
{
	struct xfs_mount	*mp = tp->t_mountp;

	if (!mp->m_metadirip) {
		xfs_fs_mark_sick(mp, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

	return xfs_metadir_load(tp, mp->m_metadirip, "rtgroups",
			XFS_METAFILE_DIR, &mp->m_rtdirip);
}

/* Check superblock fields for a read or a write. */
static xfs_failaddr_t
xfs_rtsb_verify_common(
	struct xfs_buf		*bp)
{
	struct xfs_rtsb		*rsb = bp->b_addr;

	if (!xfs_verify_magic(bp, rsb->rsb_magicnum))
		return __this_address;
	if (rsb->rsb_pad)
		return __this_address;

	/* Everything to the end of the fs block must be zero */
	if (memchr_inv(rsb + 1, 0, BBTOB(bp->b_length) - sizeof(*rsb)))
		return __this_address;

	return NULL;
}

/* Check superblock fields for a read or revalidation. */
static inline xfs_failaddr_t
xfs_rtsb_verify_all(
	struct xfs_buf		*bp)
{
	struct xfs_rtsb		*rsb = bp->b_addr;
	struct xfs_mount	*mp = bp->b_mount;
	xfs_failaddr_t		fa;

	fa = xfs_rtsb_verify_common(bp);
	if (fa)
		return fa;

	if (memcmp(&rsb->rsb_fname, &mp->m_sb.sb_fname, XFSLABEL_MAX))
		return __this_address;
	if (!uuid_equal(&rsb->rsb_uuid, &mp->m_sb.sb_uuid))
		return __this_address;
	if (!uuid_equal(&rsb->rsb_meta_uuid, &mp->m_sb.sb_meta_uuid))
		return  __this_address;

	return NULL;
}

static void
xfs_rtsb_read_verify(
	struct xfs_buf		*bp)
{
	xfs_failaddr_t		fa;

	if (!xfs_buf_verify_cksum(bp, XFS_RTSB_CRC_OFF)) {
		xfs_verifier_error(bp, -EFSBADCRC, __this_address);
		return;
	}

	fa = xfs_rtsb_verify_all(bp);
	if (fa)
		xfs_verifier_error(bp, -EFSCORRUPTED, fa);
}

static void
xfs_rtsb_write_verify(
	struct xfs_buf		*bp)
{
	xfs_failaddr_t		fa;

	fa = xfs_rtsb_verify_common(bp);
	if (fa) {
		xfs_verifier_error(bp, -EFSCORRUPTED, fa);
		return;
	}

	xfs_buf_update_cksum(bp, XFS_RTSB_CRC_OFF);
}

const struct xfs_buf_ops xfs_rtsb_buf_ops = {
	.name		= "xfs_rtsb",
	.magic		= { 0, cpu_to_be32(XFS_RTSB_MAGIC) },
	.verify_read	= xfs_rtsb_read_verify,
	.verify_write	= xfs_rtsb_write_verify,
	.verify_struct	= xfs_rtsb_verify_all,
};

/* Update a realtime superblock from the primary fs super */
void
xfs_update_rtsb(
	struct xfs_buf		*rtsb_bp,
	const struct xfs_buf	*sb_bp)
{
	const struct xfs_dsb	*dsb = sb_bp->b_addr;
	struct xfs_rtsb		*rsb = rtsb_bp->b_addr;
	const uuid_t		*meta_uuid;

	rsb->rsb_magicnum = cpu_to_be32(XFS_RTSB_MAGIC);

	rsb->rsb_pad = 0;
	memcpy(&rsb->rsb_fname, &dsb->sb_fname, XFSLABEL_MAX);

	memcpy(&rsb->rsb_uuid, &dsb->sb_uuid, sizeof(rsb->rsb_uuid));

	/*
	 * The metadata uuid is the fs uuid if the metauuid feature is not
	 * enabled.
	 */
	if (dsb->sb_features_incompat &
				cpu_to_be32(XFS_SB_FEAT_INCOMPAT_META_UUID))
		meta_uuid = &dsb->sb_meta_uuid;
	else
		meta_uuid = &dsb->sb_uuid;
	memcpy(&rsb->rsb_meta_uuid, meta_uuid, sizeof(rsb->rsb_meta_uuid));
}

/*
 * Update the realtime superblock from a filesystem superblock and log it to
 * the given transaction.
 */
struct xfs_buf *
xfs_log_rtsb(
	struct xfs_trans	*tp,
	const struct xfs_buf	*sb_bp)
{
	struct xfs_buf		*rtsb_bp;

	if (!xfs_has_rtsb(tp->t_mountp))
		return NULL;

	rtsb_bp = xfs_trans_getrtsb(tp);
	if (!rtsb_bp) {
		/*
		 * It's possible for the rtgroups feature to be enabled but
		 * there is no incore rt superblock buffer if the rt geometry
		 * was specified at mkfs time but the rt section has not yet
		 * been attached.  In this case, rblocks must be zero.
		 */
		ASSERT(tp->t_mountp->m_sb.sb_rblocks == 0);
		return NULL;
	}

	xfs_update_rtsb(rtsb_bp, sb_bp);
	xfs_trans_ordered_buf(tp, rtsb_bp);
	return rtsb_bp;
}
