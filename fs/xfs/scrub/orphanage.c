// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_inode.h"
#include "xfs_ialloc.h"
#include "xfs_quota.h"
#include "xfs_trans_space.h"
#include "xfs_dir2.h"
#include "xfs_icache.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/repair.h"
#include "scrub/trace.h"
#include "scrub/orphanage.h"

#include <linux/namei.h>

/* Make the orphanage owned by root. */
STATIC int
xrep_chown_orphanage(
	struct xfs_scrub	*sc,
	struct xfs_inode	*dp)
{
	struct xfs_trans	*tp;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_dquot	*udqp = NULL, *gdqp = NULL, *pdqp = NULL;
	struct xfs_dquot	*oldu = NULL, *oldg = NULL, *oldp = NULL;
	struct inode		*inode = VFS_I(dp);
	int			error;

	error = xfs_qm_vop_dqalloc(dp, GLOBAL_ROOT_UID, GLOBAL_ROOT_GID, 0,
			XFS_QMOPT_QUOTALL, &udqp, &gdqp, &pdqp);
	if (error)
		return error;

	error = xfs_trans_alloc_ichange(dp, udqp, gdqp, pdqp, true, &tp);
	if (error)
		goto out_dqrele;

	/*
	 * Always clear setuid/setgid on the orphanage since we don't normally
	 * want that functionality on this directory and xfs_repair doesn't
	 * create it this way either.  Leave the other access bits unchanged.
	 */
	inode->i_mode &= ~(S_ISUID | S_ISGID);

	/*
	 * Change the ownerships and register quota modifications
	 * in the transaction.
	 */
	if (!uid_eq(inode->i_uid, GLOBAL_ROOT_UID)) {
		if (XFS_IS_UQUOTA_ON(mp))
			oldu = xfs_qm_vop_chown(tp, dp, &dp->i_udquot, udqp);
		inode->i_uid = GLOBAL_ROOT_UID;
	}
	if (!gid_eq(inode->i_gid, GLOBAL_ROOT_GID)) {
		if (XFS_IS_GQUOTA_ON(mp))
			oldg = xfs_qm_vop_chown(tp, dp, &dp->i_gdquot, gdqp);
		inode->i_gid = GLOBAL_ROOT_GID;
	}
	if (dp->i_projid != 0) {
		if (XFS_IS_PQUOTA_ON(mp))
			oldp = xfs_qm_vop_chown(tp, dp, &dp->i_pdquot, pdqp);
		dp->i_projid = 0;
	}

	dp->i_diflags &= ~(XFS_DIFLAG_REALTIME | XFS_DIFLAG_RTINHERIT);
	xfs_trans_log_inode(tp, dp, XFS_ILOG_CORE);

	XFS_STATS_INC(mp, xs_ig_attrchg);

	if (xfs_has_wsync(mp))
		xfs_trans_set_sync(tp);
	error = xfs_trans_commit(tp);

	xfs_qm_dqrele(oldu);
	xfs_qm_dqrele(oldg);
	xfs_qm_dqrele(oldp);

out_dqrele:
	xfs_qm_dqrele(udqp);
	xfs_qm_dqrele(gdqp);
	xfs_qm_dqrele(pdqp);
	return error;
}

#define ORPHANAGE	"lost+found"

/* Create the orphanage directory, and set sc->orphanage to it. */
int
xrep_orphanage_create(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	struct dentry		*root_dentry, *orphanage_dentry;
	struct inode		*root_inode = VFS_I(sc->mp->m_rootip);
	struct inode		*orphanage_inode;
	int			error;

	if (xfs_is_shutdown(mp))
		return -EIO;
	if (xfs_is_readonly(mp)) {
		sc->orphanage = NULL;
		return 0;
	}

	ASSERT(sc->tp == NULL);
	ASSERT(sc->orphanage == NULL);

	/* Find the dentry for the root directory... */
	root_dentry = d_find_alias(root_inode);
	if (!root_dentry) {
		error = -EFSCORRUPTED;
		goto out;
	}

	/* ...which is a directory, right? */
	if (!d_is_dir(root_dentry)) {
		error = -EFSCORRUPTED;
		goto out_dput_root;
	}

	/* Try to find the orphanage directory. */
	inode_lock_nested(root_inode, I_MUTEX_PARENT);
	orphanage_dentry = lookup_one_len(ORPHANAGE, root_dentry,
			strlen(ORPHANAGE));
	if (IS_ERR(orphanage_dentry)) {
		error = PTR_ERR(orphanage_dentry);
		goto out_unlock_root;
	}

	/*
	 * Nothing found?  Call mkdir to create the orphanage.  Create the
	 * directory without group or other-user access because we're live and
	 * someone could have been relying partly on minimal access to a parent
	 * directory to control access to a file we put in here.
	 */
	if (d_really_is_negative(orphanage_dentry)) {
		error = vfs_mkdir(&init_user_ns, root_inode, orphanage_dentry,
				0700);
		if (error)
			goto out_dput_orphanage;
	}

	/* Not a directory? Bail out. */
	if (!d_is_dir(orphanage_dentry)) {
		error = -ENOTDIR;
		goto out_dput_orphanage;
	}

	/*
	 * Grab a reference to the orphanage.  This /should/ succeed since
	 * we hold the root directory locked and therefore nobody can delete
	 * the orphanage.
	 */
	orphanage_inode = igrab(d_inode(orphanage_dentry));
	if (!orphanage_inode) {
		error = -ENOENT;
		goto out_dput_orphanage;
	}

	/* Make sure the orphanage is owned by root. */
	error = xrep_chown_orphanage(sc, XFS_I(orphanage_inode));
	if (error)
		goto out_dput_orphanage;

	/* Stash the reference for later and bail out. */
	sc->orphanage = XFS_I(orphanage_inode);
	sc->orphanage_ilock_flags = 0;

out_dput_orphanage:
	dput(orphanage_dentry);
out_unlock_root:
	inode_unlock(VFS_I(sc->mp->m_rootip));
out_dput_root:
	dput(root_dentry);
out:
	return error;
}

void
xrep_orphanage_ilock(
	struct xfs_scrub	*sc,
	unsigned int		ilock_flags)
{
	sc->orphanage_ilock_flags |= ilock_flags;
	xfs_ilock(sc->orphanage, ilock_flags);
}

bool
xrep_orphanage_ilock_nowait(
	struct xfs_scrub	*sc,
	unsigned int		ilock_flags)
{
	if (xfs_ilock_nowait(sc->orphanage, ilock_flags)) {
		sc->orphanage_ilock_flags |= ilock_flags;
		return true;
	}

	return false;
}

void
xrep_orphanage_iunlock(
	struct xfs_scrub	*sc,
	unsigned int		ilock_flags)
{
	xfs_iunlock(sc->orphanage, ilock_flags);
	sc->orphanage_ilock_flags &= ~ilock_flags;
}

/* Grab the IOLOCK of the orphanage and sc->ip. */
int
xrep_orphanage_iolock_two(
	struct xfs_scrub	*sc)
{
	int			error = 0;

	while (true) {
		if (xchk_should_terminate(sc, &error))
			return error;

		/*
		 * Normal XFS takes the IOLOCK before grabbing a transaction.
		 * Scrub holds a transaction, which means that we can't block
		 * on either IOLOCK.
		 */
		if (xrep_orphanage_ilock_nowait(sc, XFS_IOLOCK_EXCL)) {
			if (xchk_ilock_nowait(sc, XFS_IOLOCK_EXCL))
				break;
			xrep_orphanage_iunlock(sc, XFS_IOLOCK_EXCL);
		}
		delay(1);
	}

	return 0;
}

/* Compute block reservation needed to add sc->ip to the orphanage. */
void
xrep_orphanage_compute_blkres(
	struct xfs_scrub		*sc,
	struct xrep_orphanage_req	*orph)
{
	struct xfs_mount		*mp = sc->mp;
	bool				isdir = S_ISDIR(VFS_I(sc->ip)->i_mode);

	orph->sc = sc;
	orph->orphanage_blkres = XFS_LINK_SPACE_RES(mp, MAXNAMELEN);
	orph->child_blkres = isdir ? XFS_RENAME_SPACE_RES(mp, 2) : 0;
}

/*
 * Compute the xfs_name for the directory entry that we're adding to the
 * orphanage.  Caller must have the IOLOCK of the orphanage and sc->ip.
 */
int
xrep_orphanage_compute_name(
	struct xrep_orphanage_req	*orph,
	unsigned char			*namebuf)
{
	struct xfs_name			*xname = &orph->xname;
	struct xfs_scrub		*sc = orph->sc;
	xfs_ino_t			ino;
	unsigned int			incr = 0;
	int				error = 0;

	xname->name = namebuf;
	xname->len = snprintf(namebuf, MAXNAMELEN, "%llu", sc->ip->i_ino);
	xname->type = xfs_mode_to_ftype(VFS_I(sc->ip)->i_mode);

	/* Make sure the filename is unique in the lost+found. */
	error = xfs_dir_lookup(sc->tp, sc->orphanage, xname, &ino, NULL);
	while (error == 0 && incr < 10000) {
		xname->len = snprintf(namebuf, MAXNAMELEN, "%llu.%u",
				sc->ip->i_ino, ++incr);
		error = xfs_dir_lookup(sc->tp, sc->orphanage, xname, &ino,
				NULL);
	}
	if (error == 0) {
		/* We already have 10,000 entries in the orphanage? */
		return -EFSCORRUPTED;
	}

	if (error != -ENOENT)
		return error;
	return 0;
}

/*
 * Prepare to send a child to the orphanage.
 *
 * Reserve more space in the transaction, take the ILOCKs of the orphanage and
 * sc->ip, join them to the transaction, and reserve quota to reparent the
 * latter.
 */
int
xrep_orphanage_adoption_prep(
	struct xrep_orphanage_req	*orph)
{
	struct xfs_scrub		*sc = orph->sc;
	int				error;

	/*
	 * Reserve space to the transaction to handle expansion of both the
	 * orphanage and the child directory.
	 */
	error = xfs_trans_reserve_more(sc->tp,
			orph->orphanage_blkres + orph->child_blkres, 0);
	if (error)
		return error;

	xfs_lock_two_inodes(sc->orphanage, XFS_ILOCK_EXCL,
			    sc->ip, XFS_ILOCK_EXCL);
	sc->ilock_flags |= XFS_ILOCK_EXCL;
	sc->orphanage_ilock_flags |= XFS_ILOCK_EXCL;

	xfs_trans_ijoin(sc->tp, sc->orphanage, 0);
	xfs_trans_ijoin(sc->tp, sc->ip, 0);

	/*
	 * Reserve enough quota in the orphan directory to add the new name.
	 * Normally the orphanage should have user/group/project ids of zero
	 * and hence is not subject to quota enforcement, but we're allowed to
	 * exceed quota to reattach disconnected parts of the directory tree.
	 */
	error = xfs_trans_reserve_quota_nblks(sc->tp, sc->orphanage,
			orph->orphanage_blkres, 0, true);
	if (error)
		return error;

	/*
	 * Reserve enough quota in the child directory to change dotdot.
	 * Here we're also allowed to exceed file quota to repair inconsistent
	 * metadata.
	 */
	if (orph->child_blkres) {
		error = xfs_trans_reserve_quota_nblks(sc->tp, sc->ip,
				orph->child_blkres, 0, true);
		if (error)
			return error;
	}

	return 0;
}

/*
 * Make sure the dcache does not have a positive dentry for the name we've
 * chosen.  The caller should have checked with the ondisk directory, so any
 * discrepancy is a sign that something is seriously wrong.
 */
static int
xrep_orphanage_check_dcache(
	struct xrep_orphanage_req	*orph)
{
	struct qstr			qname = QSTR_INIT(orph->xname.name,
							  orph->xname.len);
	struct dentry			*d_orphanage, *d_child, *dentry;
	int				error = 0;

	d_orphanage = d_find_alias(VFS_I(orph->sc->orphanage));
	if (!d_orphanage)
		return 0;

	d_child = d_hash_and_lookup(d_orphanage, &qname);
	if (d_child) {
		trace_xrep_orphanage_check_child(orph->sc->mp, d_child);

		if (d_is_positive(d_child)) {
			ASSERT(d_is_negative(d_child));
			error = -EFSCORRUPTED;
		}

		dput(d_child);
	}

	dput(d_orphanage);
	if (error)
		return error;

	/*
	 * Do we need to update d_parent of the dentry for the file being
	 * repaired?  In theory there shouldn't be one since the file had
	 * nonzero nlink but wasn't connected to any parent dir.
	 */
	dentry = d_find_alias(VFS_I(orph->sc->ip));
	if (dentry) {
		trace_xrep_orphanage_check_dentry(orph->sc->mp, d_child);
		ASSERT(dentry->d_parent == NULL);

		dput(dentry);
		return -EFSCORRUPTED;
	}

	return 0;
}

/*
 * Remove all negative dentries from the dcache.  There should not be any
 * positive entries, since we've maintained our lock on the orphanage
 * directory.
 */
static int
xrep_orphanage_zap_dcache(
	struct xrep_orphanage_req	*orph)
{
	struct qstr			qname = QSTR_INIT(orph->xname.name,
							  orph->xname.len);
	struct dentry			*d_orphanage, *d_child;
	int				error = 0;

	d_orphanage = d_find_alias(VFS_I(orph->sc->orphanage));
	if (!d_orphanage)
		return 0;

	d_child = d_hash_and_lookup(d_orphanage, &qname);
	while (d_child != NULL) {
		trace_xrep_orphanage_zap_child(orph->sc->mp, d_child);

		ASSERT(d_is_negative(d_child));
		d_invalidate(d_child);
		dput(d_child);
		d_child = d_lookup(d_orphanage, &qname);
	}

	dput(d_orphanage);
	return error;
}

/*
 * Move the current file to the orphanage.
 *
 * The caller must hold the IOLOCKs and the ILOCKs for both sc->ip and the
 * orphanage.  The directory entry name must have been computed, and quota
 * reserved.  The function returns with both inodes joined and ILOCKed to the
 * transaction.
 */
int
xrep_orphanage_adopt(
	struct xrep_orphanage_req	*orph)
{
	struct xfs_scrub		*sc = orph->sc;
	struct xfs_name			*xname = &orph->xname;
	bool				isdir = S_ISDIR(VFS_I(sc->ip)->i_mode);
	int				error;

	trace_xrep_orphanage_adopt(sc->orphanage, &orph->xname, sc->ip->i_ino);

	error = xrep_orphanage_check_dcache(orph);
	if (error)
		return error;

	/*
	 * Create the new name in the orphanage, and bump the link count of
	 * the orphanage if we just added a directory.
	 */
	error = xfs_dir_createname(sc->tp, sc->orphanage, xname, sc->ip->i_ino,
			orph->orphanage_blkres);
	if (error)
		return error;

	xfs_trans_ichgtime(sc->tp, sc->orphanage,
			XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
	if (isdir)
		xfs_bumplink(sc->tp, sc->orphanage);
	xfs_trans_log_inode(sc->tp, sc->orphanage, XFS_ILOG_CORE);

	if (isdir) {
		/* Replace the dotdot entry in the child directory. */
		error = xfs_dir_replace(sc->tp, sc->ip, &xfs_name_dotdot,
				sc->orphanage->i_ino, orph->child_blkres);
		if (error)
			return error;
	}

	return xrep_orphanage_zap_dcache(orph);
}

/* Release the orphanage. */
void
xrep_orphanage_rele(
	struct xfs_scrub	*sc)
{
	if (!sc->orphanage)
		return;

	if (sc->orphanage_ilock_flags)
		xfs_iunlock(sc->orphanage, sc->orphanage_ilock_flags);

	xchk_irele(sc, sc->orphanage);
	sc->orphanage = NULL;
}
