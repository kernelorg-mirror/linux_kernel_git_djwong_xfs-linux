// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include <linux/iversion.h>
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_inode_util.h"
#include "xfs_trans.h"
#include "xfs_ialloc.h"
#include "xfs_error.h"
#include "xfs_trace.h"
#include "xfs_inode_item.h"
#include "xfs_imeta.h"

uint16_t
xfs_flags2diflags(
	struct xfs_inode	*ip,
	unsigned int		xflags)
{
	/* can't set PREALLOC this way, just preserve it */
	uint16_t		di_flags =
		(ip->i_d.di_flags & XFS_DIFLAG_PREALLOC);

	if (xflags & FS_XFLAG_IMMUTABLE)
		di_flags |= XFS_DIFLAG_IMMUTABLE;
	if (xflags & FS_XFLAG_APPEND)
		di_flags |= XFS_DIFLAG_APPEND;
	if (xflags & FS_XFLAG_SYNC)
		di_flags |= XFS_DIFLAG_SYNC;
	if (xflags & FS_XFLAG_NOATIME)
		di_flags |= XFS_DIFLAG_NOATIME;
	if (xflags & FS_XFLAG_NODUMP)
		di_flags |= XFS_DIFLAG_NODUMP;
	if (xflags & FS_XFLAG_NODEFRAG)
		di_flags |= XFS_DIFLAG_NODEFRAG;
	if (xflags & FS_XFLAG_FILESTREAM)
		di_flags |= XFS_DIFLAG_FILESTREAM;
	if (S_ISDIR(VFS_I(ip)->i_mode)) {
		if (xflags & FS_XFLAG_RTINHERIT)
			di_flags |= XFS_DIFLAG_RTINHERIT;
		if (xflags & FS_XFLAG_NOSYMLINKS)
			di_flags |= XFS_DIFLAG_NOSYMLINKS;
		if (xflags & FS_XFLAG_EXTSZINHERIT)
			di_flags |= XFS_DIFLAG_EXTSZINHERIT;
		if (xflags & FS_XFLAG_PROJINHERIT)
			di_flags |= XFS_DIFLAG_PROJINHERIT;
	} else if (S_ISREG(VFS_I(ip)->i_mode)) {
		if (xflags & FS_XFLAG_REALTIME)
			di_flags |= XFS_DIFLAG_REALTIME;
		if (xflags & FS_XFLAG_EXTSIZE)
			di_flags |= XFS_DIFLAG_EXTSIZE;
	}

	return di_flags;
}

uint64_t
xfs_flags2diflags2(
	struct xfs_inode	*ip,
	unsigned int		xflags)
{
	uint64_t		di_flags2 =
		(ip->i_d.di_flags2 & XFS_DIFLAG2_REFLINK);

	if (xflags & FS_XFLAG_DAX)
		di_flags2 |= XFS_DIFLAG2_DAX;
	if (xflags & FS_XFLAG_COWEXTSIZE)
		di_flags2 |= XFS_DIFLAG2_COWEXTSIZE;

	return di_flags2;
}

uint32_t
xfs_dic2xflags(
	uint16_t		di_flags,
	uint64_t		di_flags2,
	bool			has_attr)
{
	uint			flags = 0;

	if (di_flags & XFS_DIFLAG_ANY) {
		if (di_flags & XFS_DIFLAG_REALTIME)
			flags |= FS_XFLAG_REALTIME;
		if (di_flags & XFS_DIFLAG_PREALLOC)
			flags |= FS_XFLAG_PREALLOC;
		if (di_flags & XFS_DIFLAG_IMMUTABLE)
			flags |= FS_XFLAG_IMMUTABLE;
		if (di_flags & XFS_DIFLAG_APPEND)
			flags |= FS_XFLAG_APPEND;
		if (di_flags & XFS_DIFLAG_SYNC)
			flags |= FS_XFLAG_SYNC;
		if (di_flags & XFS_DIFLAG_NOATIME)
			flags |= FS_XFLAG_NOATIME;
		if (di_flags & XFS_DIFLAG_NODUMP)
			flags |= FS_XFLAG_NODUMP;
		if (di_flags & XFS_DIFLAG_RTINHERIT)
			flags |= FS_XFLAG_RTINHERIT;
		if (di_flags & XFS_DIFLAG_PROJINHERIT)
			flags |= FS_XFLAG_PROJINHERIT;
		if (di_flags & XFS_DIFLAG_NOSYMLINKS)
			flags |= FS_XFLAG_NOSYMLINKS;
		if (di_flags & XFS_DIFLAG_EXTSIZE)
			flags |= FS_XFLAG_EXTSIZE;
		if (di_flags & XFS_DIFLAG_EXTSZINHERIT)
			flags |= FS_XFLAG_EXTSZINHERIT;
		if (di_flags & XFS_DIFLAG_NODEFRAG)
			flags |= FS_XFLAG_NODEFRAG;
		if (di_flags & XFS_DIFLAG_FILESTREAM)
			flags |= FS_XFLAG_FILESTREAM;
	}

	if (di_flags2 & XFS_DIFLAG2_ANY) {
		if (di_flags2 & XFS_DIFLAG2_DAX)
			flags |= FS_XFLAG_DAX;
		if (di_flags2 & XFS_DIFLAG2_COWEXTSIZE)
			flags |= FS_XFLAG_COWEXTSIZE;
	}

	if (has_attr)
		flags |= FS_XFLAG_HASATTR;

	return flags;
}

/*
 * Project quota id helpers (previously projid was 16bit only and using two
 * 16bit values to hold new 32bit projid was chosen to retain compatibility
 * with "old" filesystems).
 */
prid_t
xfs_get_projid(
	struct xfs_inode	*ip)
{
	return (prid_t)ip->i_d.di_projid_hi << 16 | ip->i_d.di_projid_lo;
}

void
xfs_set_projid(
	struct xfs_inode	*ip,
	prid_t			projid)
{
	ip->i_d.di_projid_hi = (uint16_t) (projid >> 16);
	ip->i_d.di_projid_lo = (uint16_t) (projid & 0xffff);
}

prid_t
xfs_get_initial_prid(
	struct xfs_inode	*dp)
{
	if (dp->i_d.di_flags & XFS_DIFLAG_PROJINHERIT)
		return xfs_get_projid(dp);

	return XFS_PROJID_DEFAULT;
}

/*
 * Initialize a newly allocated inode with the given arguments.  Heritable
 * inode properties will be copied from the parent if one is supplied and the
 * appropriate inode flags are set on the parent.
 */
void
xfs_inode_init(
	struct xfs_trans		*tp,
	const struct xfs_ialloc_args	*args,
	struct xfs_inode		*ip)
{
	struct xfs_inode		*pip = args->pip;
	struct inode			*inode = VFS_I(ip);
	int				times;
	uint				flags;

	/*
	 * We always convert v1 inodes to v2 now - we only support filesystems
	 * with >= v2 inode capability, so there is no reason for ever leaving
	 * an inode in v1 format.
	 */
	if (ip->i_d.di_version == 1)
		ip->i_d.di_version = 2;

	inode->i_mode = args->mode;
	set_nlink(inode, args->nlink);
	ip->i_d.di_uid = args->uid;
	ip->i_d.di_gid = args->gid;
	inode->i_rdev = args->rdev;
	xfs_set_projid(ip, args->prid);

	if (pip && XFS_INHERIT_GID(pip)) {
		ip->i_d.di_gid = pip->i_d.di_gid;
		if ((VFS_I(pip)->i_mode & S_ISGID) && S_ISDIR(args->mode))
			inode->i_mode |= S_ISGID;
	}

	/*
	 * If the group ID of the new file does not match the effective group
	 * ID or one of the supplementary group IDs, the S_ISGID bit is cleared
	 * (and only if the irix_sgid_inherit compatibility variable is set).
	 */
	if ((irix_sgid_inherit) &&
	    (inode->i_mode & S_ISGID) &&
	    (!in_group_p(xfs_gid_to_kgid(ip->i_d.di_gid))))
		inode->i_mode &= ~S_ISGID;

	ip->i_d.di_size = 0;
	ip->i_d.di_nextents = 0;
	ASSERT(ip->i_d.di_nblocks == 0);

	times = XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG | XFS_ICHGTIME_ACCESS;
	ip->i_d.di_extsize = 0;
	ip->i_d.di_dmevmask = 0;
	ip->i_d.di_dmstate = 0;
	ip->i_d.di_flags = 0;

	if (ip->i_d.di_version == 3) {
		inode_set_iversion(inode, 1);
		ip->i_d.di_flags2 = 0;
		ip->i_d.di_cowextsize = 0;
		times |= XFS_ICHGTIME_CREATE;
	}

	xfs_trans_ichgtime(tp, ip, times);

	flags = XFS_ILOG_CORE;
	switch (args->mode & S_IFMT) {
	case S_IFIFO:
	case S_IFCHR:
	case S_IFBLK:
	case S_IFSOCK:
		ip->i_d.di_format = XFS_DINODE_FMT_DEV;
		ip->i_df.if_flags = 0;
		flags |= XFS_ILOG_DEV;
		break;
	case S_IFREG:
	case S_IFDIR:
		if (pip && (pip->i_d.di_flags & XFS_DIFLAG_ANY)) {
			uint		di_flags = 0;

			if (S_ISDIR(args->mode)) {
				if (pip->i_d.di_flags & XFS_DIFLAG_RTINHERIT)
					di_flags |= XFS_DIFLAG_RTINHERIT;
				if (pip->i_d.di_flags & XFS_DIFLAG_EXTSZINHERIT) {
					di_flags |= XFS_DIFLAG_EXTSZINHERIT;
					ip->i_d.di_extsize = pip->i_d.di_extsize;
				}
				if (pip->i_d.di_flags & XFS_DIFLAG_PROJINHERIT)
					di_flags |= XFS_DIFLAG_PROJINHERIT;
			} else if (S_ISREG(args->mode)) {
				if (pip->i_d.di_flags & XFS_DIFLAG_RTINHERIT)
					di_flags |= XFS_DIFLAG_REALTIME;
				if (pip->i_d.di_flags & XFS_DIFLAG_EXTSZINHERIT) {
					di_flags |= XFS_DIFLAG_EXTSIZE;
					ip->i_d.di_extsize = pip->i_d.di_extsize;
				}
			}
			if ((pip->i_d.di_flags & XFS_DIFLAG_NOATIME) &&
			    xfs_inherit_noatime)
				di_flags |= XFS_DIFLAG_NOATIME;
			if ((pip->i_d.di_flags & XFS_DIFLAG_NODUMP) &&
			    xfs_inherit_nodump)
				di_flags |= XFS_DIFLAG_NODUMP;
			if ((pip->i_d.di_flags & XFS_DIFLAG_SYNC) &&
			    xfs_inherit_sync)
				di_flags |= XFS_DIFLAG_SYNC;
			if ((pip->i_d.di_flags & XFS_DIFLAG_NOSYMLINKS) &&
			    xfs_inherit_nosymlinks)
				di_flags |= XFS_DIFLAG_NOSYMLINKS;
			if ((pip->i_d.di_flags & XFS_DIFLAG_NODEFRAG) &&
			    xfs_inherit_nodefrag)
				di_flags |= XFS_DIFLAG_NODEFRAG;
			if (pip->i_d.di_flags & XFS_DIFLAG_FILESTREAM)
				di_flags |= XFS_DIFLAG_FILESTREAM;

			ip->i_d.di_flags |= di_flags;
		}
		if (pip &&
		    (pip->i_d.di_flags2 & XFS_DIFLAG2_ANY) &&
		    pip->i_d.di_version == 3 &&
		    ip->i_d.di_version == 3) {
			uint64_t	di_flags2 = 0;

			if (pip->i_d.di_flags2 & XFS_DIFLAG2_COWEXTSIZE) {
				di_flags2 |= XFS_DIFLAG2_COWEXTSIZE;
				ip->i_d.di_cowextsize = pip->i_d.di_cowextsize;
			}
			if (pip->i_d.di_flags2 & XFS_DIFLAG2_DAX)
				di_flags2 |= XFS_DIFLAG2_DAX;

			ip->i_d.di_flags2 |= di_flags2;
		}
		/* FALLTHROUGH */
	case S_IFLNK:
		ip->i_d.di_format = XFS_DINODE_FMT_EXTENTS;
		ip->i_df.if_flags = XFS_IFEXTENTS;
		ip->i_df.if_bytes = 0;
		ip->i_df.if_u1.if_root = NULL;
		break;
	default:
		ASSERT(0);
	}
	/*
	 * Attribute fork settings for new inode.
	 */
	ip->i_d.di_aformat = XFS_DINODE_FMT_EXTENTS;
	ip->i_d.di_anextents = 0;

	/*
	 * Log the new values stuffed into the inode.
	 */
	xfs_trans_ijoin(tp, ip, XFS_ILOCK_EXCL);
	xfs_trans_log_inode(tp, ip, flags);

	/* now that we have an i_mode we can setup the inode structure */
	xfs_setup_inode(ip);
}

/*
 * Allocate an inode on disk and return a copy of its in-core version.
 * The in-core inode is locked exclusively.  Set mode, nlink, and rdev
 * appropriately within the inode.  The uid and gid for the inode are
 * set according to the contents of the given cred structure.
 *
 * Use xfs_dialloc() to allocate the on-disk inode. If xfs_dialloc()
 * has a free inode available, call xfs_iget() to obtain the in-core
 * version of the allocated inode.  Finally, fill in the inode and
 * log its initial contents.  In this case, ialloc_context would be
 * set to NULL.
 *
 * If xfs_dialloc() does not have an available inode, it will replenish
 * its supply by doing an allocation. Since we can only do one
 * allocation within a transaction without deadlocks, we must commit
 * the current transaction before returning the inode itself.
 * In this case, therefore, we will set ialloc_context and return.
 * The caller should then commit the current transaction, start a new
 * transaction, and call xfs_ialloc() again to actually get the inode.
 *
 * To ensure that some other process does not grab the inode that
 * was allocated during the first call to xfs_ialloc(), this routine
 * also returns the [locked] bp pointing to the head of the freelist
 * as ialloc_context.  The caller should hold this buffer across
 * the commit and pass it back into this routine on the second call.
 *
 * If we are allocating quota inodes, we do not have a parent inode
 * to attach to or associate with (i.e. pip == NULL) because they
 * are not linked into the directory structure - they are attached
 * directly to the superblock - and so have no parent.
 */
STATIC int
xfs_ialloc(
	struct xfs_trans		*tp,
	const struct xfs_ialloc_args	*args,
	struct xfs_buf			**ialloc_context,
	struct xfs_inode		**ipp)
{
	struct xfs_mount		*mp = tp->t_mountp;
	struct xfs_inode		*pip = args->pip;
	struct xfs_inode		*ip;
	xfs_ino_t			ino;
	int				error;

	/*
	 * Call the space management code to pick
	 * the on-disk inode to be allocated.
	 */
	error = xfs_dialloc(tp, pip, args->mode, ialloc_context, &ino);
	if (error)
		return error;
	if (*ialloc_context || ino == NULLFSINO) {
		*ipp = NULL;
		return 0;
	}
	ASSERT(*ialloc_context == NULL);

	/*
	 * Protect against obviously corrupt allocation btree records. Later
	 * xfs_iget checks will catch re-allocation of other active in-memory
	 * and on-disk inodes. If we don't catch reallocating the parent inode
	 * here we will deadlock in xfs_iget() so we have to do these checks
	 * first.
	 */
	if ((pip && ino == pip->i_ino) || !xfs_verify_dir_ino(mp, ino)) {
		xfs_alert(mp, "Allocated a known in-use inode 0x%llx!", ino);
		return -EFSCORRUPTED;
	}

	/*
	 * Get the in-core inode with the lock held exclusively.
	 * This is because we're setting fields here we need
	 * to prevent others from looking at until we're done.
	 */
	error = xfs_ialloc_iget(tp, ino, &ip);
	if (error)
		return error;
	ASSERT(ip != NULL);

	xfs_inode_init(tp, args, ip);
	*ipp = ip;
	return 0;
}

/*
 * Allocates a new inode from disk and return a pointer to the
 * incore copy. This routine will internally commit the current
 * transaction and allocate a new one if the Space Manager needed
 * to do an allocation to replenish the inode free-list.
 *
 * This routine is designed to be called from xfs_create and
 * xfs_create_dir.
 *
 */
int
xfs_dir_ialloc(
	struct xfs_trans		**tpp,
	const struct xfs_ialloc_args	*args,
	struct xfs_inode		**ipp)
{
	struct xfs_trans		*tp;
	struct xfs_inode		*ip;
	struct xfs_buf			*ialloc_context = NULL;
	int				error;

	tp = *tpp;
	ASSERT(tp->t_flags & XFS_TRANS_PERM_LOG_RES);

	/*
	 * xfs_ialloc will return a pointer to an incore inode if
	 * the Space Manager has an available inode on the free
	 * list. Otherwise, it will do an allocation and replenish
	 * the freelist.  Since we can only do one allocation per
	 * transaction without deadlocks, we will need to commit the
	 * current transaction and start a new one.  We will then
	 * need to call xfs_ialloc again to get the inode.
	 *
	 * If xfs_ialloc did an allocation to replenish the freelist,
	 * it returns the bp containing the head of the freelist as
	 * ialloc_context. We will hold a lock on it across the
	 * transaction commit so that no other process can steal
	 * the inode(s) that we've just allocated.
	 */
	error = xfs_ialloc(tp, args, &ialloc_context, &ip);

	/*
	 * Return an error if we were unable to allocate a new inode.
	 * This should only happen if we run out of space on disk or
	 * encounter a disk error.
	 */
	if (error) {
		*ipp = NULL;
		return error;
	}
	if (!ialloc_context && !ip) {
		*ipp = NULL;
		return -ENOSPC;
	}

	/*
	 * If the AGI buffer is non-NULL, then we were unable to get an
	 * inode in one operation.  We need to commit the current
	 * transaction and call xfs_ialloc() again.  It is guaranteed
	 * to succeed the second time.
	 */
	if (ialloc_context) {
		/*
		 * Normally, xfs_trans_commit releases all the locks.
		 * We call bhold to hang on to the ialloc_context across
		 * the commit.  Holding this buffer prevents any other
		 * processes from doing any allocations in this
		 * allocation group.
		 */
		xfs_trans_bhold(tp, ialloc_context);

		error = xfs_dir_ialloc_roll(&tp);
		if (error) {
			xfs_buf_relse(ialloc_context);
			*tpp = tp;
			*ipp = NULL;
			return error;
		}
		xfs_trans_bjoin(tp, ialloc_context);

		/*
		 * Call ialloc again. Since we've locked out all
		 * other allocations in this allocation group,
		 * this call should always succeed.
		 */
		error = xfs_ialloc(tp, args, &ialloc_context, &ip);

		/*
		 * If we get an error at this point, return to the caller
		 * so that the current transaction can be aborted.
		 */
		if (error) {
			*tpp = tp;
			*ipp = NULL;
			return error;
		}
		ASSERT(!ialloc_context && ip);

	}

	*ipp = ip;
	*tpp = tp;

	return 0;
}

/*
 * Point the AGI unlinked bucket at an inode and log the results.  The caller
 * is responsible for validating the old value.
 */
STATIC int
xfs_iunlink_update_bucket(
	struct xfs_trans	*tp,
	xfs_agnumber_t		agno,
	struct xfs_buf		*agibp,
	unsigned int		bucket_index,
	xfs_agino_t		new_agino)
{
	struct xfs_agi		*agi = XFS_BUF_TO_AGI(agibp);
	xfs_agino_t		old_value;
	int			offset;

	ASSERT(xfs_verify_agino_or_null(tp->t_mountp, agno, new_agino));

	old_value = be32_to_cpu(agi->agi_unlinked[bucket_index]);
	trace_xfs_iunlink_update_bucket(tp->t_mountp, agno, bucket_index,
			old_value, new_agino);

	/*
	 * We should never find the head of the list already set to the value
	 * passed in because either we're adding or removing ourselves from the
	 * head of the list.
	 */
	if (old_value == new_agino)
		return -EFSCORRUPTED;

	agi->agi_unlinked[bucket_index] = cpu_to_be32(new_agino);
	offset = offsetof(struct xfs_agi, agi_unlinked) +
			(sizeof(xfs_agino_t) * bucket_index);
	xfs_trans_log_buf(tp, agibp, offset, offset + sizeof(xfs_agino_t) - 1);
	return 0;
}

/* Set an on-disk inode's next_unlinked pointer. */
STATIC void
xfs_iunlink_update_dinode(
	struct xfs_trans	*tp,
	xfs_agnumber_t		agno,
	struct xfs_buf		*ibp,
	struct xfs_dinode	*dip,
	struct xfs_imap		*imap,
	xfs_ino_t		ino,
	xfs_agino_t		next_agino)
{
	struct xfs_mount	*mp = tp->t_mountp;
	int			offset;

	ASSERT(xfs_verify_agino_or_null(mp, agno, next_agino));

	trace_xfs_iunlink_update_dinode(mp, agno, XFS_INO_TO_AGINO(mp, ino),
			be32_to_cpu(dip->di_next_unlinked), next_agino);

	dip->di_next_unlinked = cpu_to_be32(next_agino);
	offset = imap->im_boffset +
			offsetof(struct xfs_dinode, di_next_unlinked);

	/* need to recalc the inode CRC if appropriate */
	xfs_dinode_calc_crc(mp, dip);
	xfs_trans_inode_buf(tp, ibp);
	xfs_trans_log_buf(tp, ibp, offset, offset + sizeof(xfs_agino_t) - 1);
	xfs_inobp_check(mp, ibp);
}

/* Set an in-core inode's unlinked pointer and return the old value. */
STATIC int
xfs_iunlink_update_inode(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	xfs_agnumber_t		agno,
	xfs_agino_t		next_agino,
	xfs_agino_t		*old_next_agino)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_dinode	*dip;
	struct xfs_buf		*ibp;
	xfs_agino_t		old_value;
	int			error;

	ASSERT(xfs_verify_agino_or_null(mp, agno, next_agino));

	error = xfs_imap_to_bp(mp, tp, &ip->i_imap, &dip, &ibp, 0, 0);
	if (error)
		return error;

	/* Make sure the old pointer isn't garbage. */
	old_value = be32_to_cpu(dip->di_next_unlinked);
	if (old_value != NULLAGINO && !xfs_verify_agino(mp, agno, old_value)) {
		error = -EFSCORRUPTED;
		goto out;
	}

	/*
	 * Since we're updating a linked list, we should never find that the
	 * current pointer is the same as the new value, unless we're
	 * terminating the list.
	 */
	*old_next_agino = old_value;
	if (old_value == next_agino) {
		if (next_agino != NULLAGINO)
			error = -EFSCORRUPTED;
		goto out;
	}

	/* Ok, update the new pointer. */
	xfs_iunlink_update_dinode(tp, agno, ibp, dip, &ip->i_imap, ip->i_ino,
			next_agino);
	return 0;
out:
	xfs_trans_brelse(tp, ibp);
	return error;
}

/*
 * This is called when the inode's link count goes to 0 or we are creating a
 * tmpfile via O_TMPFILE. In the case of a tmpfile, @ignore_linkcount will be
 * set to true as the link count is dropped to zero by the VFS after we've
 * created the file successfully, so we have to add it to the unlinked list
 * while the link count is non-zero.
 *
 * We place the on-disk inode on a list in the AGI.  It will be pulled from this
 * list when the inode is freed.
 */
int
xfs_iunlink(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_agi		*agi;
	struct xfs_buf		*agibp;
	struct xfs_perag	*pag;
	xfs_agino_t		next_agino;
	xfs_agnumber_t		agno = XFS_INO_TO_AGNO(mp, ip->i_ino);
	xfs_agino_t		agino = XFS_INO_TO_AGINO(mp, ip->i_ino);
	short			bucket_index = agino % XFS_AGI_UNLINKED_BUCKETS;
	int			error;

	ASSERT(VFS_I(ip)->i_mode != 0);
	pag = xfs_perag_get(mp, agno);
	trace_xfs_iunlink(ip);

	/* Get the agi buffer first.  It ensures lock ordering on the list. */
	error = xfs_read_agi(mp, tp, agno, &agibp);
	if (error)
		goto out;
	agi = XFS_BUF_TO_AGI(agibp);

	/*
	 * Get the index into the agi hash table for the list this inode will
	 * go on.  Make sure the pointer isn't garbage and that this inode
	 * isn't already on the list.
	 */
	next_agino = be32_to_cpu(agi->agi_unlinked[bucket_index]);
	if (next_agino == agino ||
	    !xfs_verify_agino_or_null(mp, agno, next_agino)) {
		error = -EFSCORRUPTED;
		goto out;
	}

	if (next_agino != NULLAGINO) {
		xfs_agino_t	old_agino;

		/*
		 * There is already another inode in the bucket, so point this
		 * inode to the current head of the list.
		 */
		error = xfs_iunlink_update_inode(tp, ip, agno, next_agino,
				&old_agino);
		if (error)
			goto out;
		ASSERT(old_agino == NULLAGINO);

		/*
		 * agino has been unlinked, add a backref from the next inode
		 * back to agino.
		 */
		error = xfs_iunlink_add_backref(pag, agino, next_agino);
		if (error)
			goto out;
	}

	/* Point the head of the list to point to this inode. */
	error = xfs_iunlink_update_bucket(tp, agno, agibp, bucket_index, agino);
	if (error)
		goto out;
	pag->pagi_unlinked_count++;
out:
	xfs_perag_put(pag);
	return error;
}

/* Return the imap, dinode pointer, and buffer for an inode. */
STATIC int
xfs_iunlink_map_ino(
	struct xfs_trans	*tp,
	xfs_ino_t		ino,
	struct xfs_imap		*imap,
	struct xfs_dinode	**dipp,
	struct xfs_buf		**bpp)
{
	struct xfs_mount	*mp = tp->t_mountp;
	int			error;

	imap->im_blkno = 0;
	error = xfs_imap(mp, tp, ino, imap, 0);
	if (error) {
		xfs_warn(mp, "%s: xfs_imap returned error %d.",
				__func__, error);
		return error;
	}

	error = xfs_imap_to_bp(mp, tp, imap, dipp, bpp, 0, 0);
	if (error) {
		xfs_warn(mp, "%s: xfs_imap_to_bp returned error %d.",
				__func__, error);
		return error;
	}

	return 0;
}

/*
 * Walk the unlinked chain from @head_agino until we find the inode that
 * points to @target_agino.  Return the inode number, map, dinode pointer,
 * and inode cluster buffer of that inode as @ino, @imap, @dipp, and @bpp.
 *
 * @tp, @pag, @head_agino, and @target_agino are input parameters.
 * @ino, @imap, @dipp, and @bpp are all output parameters.
 *
 * Do not call this function if @target_agino is the head of the list.
 */
STATIC int
xfs_iunlink_map_prev(
	struct xfs_trans	*tp,
	struct xfs_perag	*pag,
	xfs_agino_t		head_agino,
	xfs_agino_t		target_agino,
	xfs_ino_t		*ino,
	struct xfs_imap		*imap,
	struct xfs_dinode	**dipp,
	struct xfs_buf		**bpp)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_buf		*last_ibp = NULL;
	struct xfs_dinode	*last_dip;
	xfs_ino_t		next_ino = NULLFSINO;
	xfs_agino_t		next_agino;
	int			error;

	ASSERT(head_agino != target_agino);

	/* See if our backref cache can find it faster. */
	next_agino = xfs_iunlink_lookup_backref(pag, target_agino);
	if (next_agino != NULLAGINO) {
		next_ino = XFS_AGINO_TO_INO(mp, pag->pag_agno, next_agino);
		error = xfs_iunlink_map_ino(tp, next_ino, imap, &last_dip,
				&last_ibp);
		if (error)
			return error;
		goto out;
	}

	next_agino = head_agino;
	while (next_agino != target_agino) {
		xfs_agino_t	unlinked_agino;

		if (last_ibp)
			xfs_trans_brelse(tp, last_ibp);

		next_ino = XFS_AGINO_TO_INO(mp, pag->pag_agno, next_agino);
		error = xfs_iunlink_map_ino(tp, next_ino, imap, &last_dip,
				&last_ibp);
		if (error)
			return error;

		unlinked_agino = be32_to_cpu(last_dip->di_next_unlinked);
		/*
		 * Make sure this pointer is valid and isn't an obvious
		 * infinite loop.
		 */
		if (!xfs_verify_agino(mp, pag->pag_agno, unlinked_agino) ||
		    next_agino == unlinked_agino) {
			XFS_CORRUPTION_ERROR(__func__,
					XFS_ERRLEVEL_LOW, mp,
					last_dip, sizeof(*last_dip));
			error = -EFSCORRUPTED;
			return error;
		}
		next_agino = unlinked_agino;
	}

out:
	/* Should never happen, but don't return garbage. */
	if (next_ino == NULLFSINO)
		return -EFSCORRUPTED;

	*ino = next_ino;
	*dipp = last_dip;
	*bpp = last_ibp;
	return 0;
}

/*
 * Pull the on-disk inode from the AGI unlinked list.
 */
int
xfs_iunlink_remove(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_agi		*agi;
	struct xfs_buf		*agibp;
	struct xfs_buf		*last_ibp;
	struct xfs_dinode	*last_dip = NULL;
	struct xfs_perag	*pag;
	xfs_ino_t		prev_ino;
	xfs_agnumber_t		agno = XFS_INO_TO_AGNO(mp, ip->i_ino);
	xfs_agino_t		agino = XFS_INO_TO_AGINO(mp, ip->i_ino);
	xfs_agino_t		next_agino;
	xfs_agino_t		head_agino;
	short			bucket_index = agino % XFS_AGI_UNLINKED_BUCKETS;
	int			error;

	pag = xfs_perag_get(mp, agno);
	trace_xfs_iunlink_remove(ip);

	/* Get the agi buffer first.  It ensures lock ordering on the list. */
	error = xfs_read_agi(mp, tp, agno, &agibp);
	if (error)
		goto out;
	agi = XFS_BUF_TO_AGI(agibp);

	/*
	 * Get the index into the agi hash table for the list this inode will
	 * go on.  Make sure the head pointer isn't garbage.
	 */
	head_agino = be32_to_cpu(agi->agi_unlinked[bucket_index]);
	if (!xfs_verify_agino(mp, agno, head_agino)) {
		XFS_CORRUPTION_ERROR(__func__, XFS_ERRLEVEL_LOW, mp,
				agi, sizeof(*agi));
		error = -EFSCORRUPTED;
		goto out;
	}

	/*
	 * Set our inode's next_unlinked pointer to NULL and then return
	 * the old pointer value so that we can update whatever was previous
	 * to us in the list to point to whatever was next in the list.
	 */
	error = xfs_iunlink_update_inode(tp, ip, agno, NULLAGINO, &next_agino);
	if (error)
		goto out;

	/*
	 * If there was a backref pointing from the next inode back to this
	 * one, remove it because we've removed this inode from the list.
	 *
	 * Later, if this inode was in the middle of the list we'll update
	 * this inode's backref to point from the next inode.
	 */
	if (next_agino != NULLAGINO) {
		error = xfs_iunlink_change_backref(pag, next_agino,
				NULLAGINO);
		if (error)
			goto out;
	}

	if (head_agino == agino) {
		/* Point the head of the list to the next unlinked inode. */
		error = xfs_iunlink_update_bucket(tp, agno, agibp, bucket_index,
				next_agino);
		if (error)
			goto out;
	} else {
		struct xfs_imap	imap;

		/* We need to search the list for the inode being freed. */
		error = xfs_iunlink_map_prev(tp, pag, head_agino, agino,
				&prev_ino, &imap, &last_dip, &last_ibp);
		if (error)
			goto out;

		/* Point the previous inode on the list to the next inode. */
		xfs_iunlink_update_dinode(tp, agno, last_ibp, last_dip, &imap,
				prev_ino, next_agino);

		/*
		 * Now we deal with the backref for this inode.  If this inode
		 * pointed at a real inode, change the backref that pointed to
		 * us to point to our old next.  If this inode was the end of
		 * the list, delete the backref that pointed to us.  Note that
		 * change_backref takes care of deleting the backref if
		 * next_agino is NULLAGINO.
		 */
		error = xfs_iunlink_change_backref(pag, agino, next_agino);
		if (error)
			goto out;
	}
	pag->pagi_unlinked_count--;
out:
	xfs_perag_put(pag);
	return error;
}

/*
 * Decrement the link count on an inode & log the change.  If this causes the
 * link count to go to zero, move the inode to AGI unlinked list so that it can
 * be freed when the last active reference goes away via xfs_inactive().
 */
int
xfs_droplink(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	xfs_trans_ichgtime(tp, ip, XFS_ICHGTIME_CHG);

	drop_nlink(VFS_I(ip));
	xfs_imeta_droplink(ip);
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);

	if (VFS_I(ip)->i_nlink)
		return 0;

	return xfs_iunlink(tp, ip);
}

/*
 * Increment the link count on an inode & log the change.
 */
int
xfs_bumplink(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	xfs_trans_ichgtime(tp, ip, XFS_ICHGTIME_CHG);

	ASSERT(ip->i_d.di_version > 1);
	inc_nlink(VFS_I(ip));
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	return 0;
}

/*
 * Free any local-format buffers sitting around before we reset to
 * extents format.
 */
static inline void
xfs_ifree_local_data(
	struct xfs_inode	*ip,
	int			whichfork)
{
	struct xfs_ifork	*ifp;

	if (XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_LOCAL)
		return;

	ifp = XFS_IFORK_PTR(ip, whichfork);
	xfs_idata_realloc(ip, -ifp->if_bytes, whichfork);
}

/* Mark an inode free on disk. */
int
xfs_dir_ifree(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	struct xfs_icluster	*xic)
{
	int			error;

	/*
	 * Pull the on-disk inode from the AGI unlinked list.
	 */
	error = xfs_iunlink_remove(tp, ip);
	if (error)
		return error;

	error = xfs_difree(tp, ip->i_ino, xic);
	if (error)
		return error;

	xfs_ifree_local_data(ip, XFS_DATA_FORK);
	xfs_ifree_local_data(ip, XFS_ATTR_FORK);

	VFS_I(ip)->i_mode = 0;		/* mark incore inode as free */
	ip->i_d.di_flags = 0;
	ip->i_d.di_flags2 = 0;
	ip->i_d.di_dmevmask = 0;
	ip->i_d.di_forkoff = 0;		/* mark the attr fork not in use */
	ip->i_d.di_format = XFS_DINODE_FMT_EXTENTS;
	ip->i_d.di_aformat = XFS_DINODE_FMT_EXTENTS;

	/* Don't attempt to replay owner changes for a deleted inode */
	ip->i_itemp->ili_fields &= ~(XFS_ILOG_AOWNER | XFS_ILOG_DOWNER);

	/*
	 * Bump the generation count so no one will be confused
	 * by reincarnations of this inode.
	 */
	VFS_I(ip)->i_generation++;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);

	return 0;
}
