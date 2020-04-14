// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_bit.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "xfs_bmap.h"
#include "xfs_quota.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans_space.h"
#include "xfs_iwalk.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/array.h"
#include "scrub/blob.h"
#include "scrub/parent.h"

/*
 * Directory Repair
 * ================
 *
 * We repair directories by reading the directory leaf blocks looking for
 * entries, truncate the entire directory fork, and reinsert all the entries.
 * Unfortunately, there's not yet a secondary copy of directory attribute data,
 * which means that if we blow up midway through there's little we can do.
 */

/* Directory entry to be restored in the new directory. */
struct xrep_dir_key {
	/* Cookie for retrieval of the dirent name. */
	xblob_cookie		name_cookie;

	/* Target inode number. */
	xfs_ino_t		ino;

	/* Hash of the dirent name. */
	unsigned int		hash;

	/* Length of the dirent name. */
	uint8_t			namelen;

	/* File type of the dirent. */
	uint8_t			ftype;
} __packed;

struct xrep_dir {
	struct xfs_scrub	*sc;

	/* Fixed-size array of xrep_dir_key structures. */
	struct xfbma		*dir_entries;

	/* Blobs containing directory entry names. */
	struct xblob		*dir_names;

	/*
	 * Potential parent of the directory we're reconstructing.  This can
	 * be NULLFSINO if we haven't found any parents; 0 if we've found too
	 * many parents during salvaging; or a regular inode number if we've
	 * found a good candidate.
	 */
	xfs_ino_t		parent_ino;
};

/*
 * Decide if we want to salvage this entry.  We don't bother with oversized
 * names or the dot entry.
 */
STATIC int
xrep_dir_want_salvage(
	struct xrep_dir		*rd,
	const char		*name,
	int			namelen,
	xfs_ino_t		ino)
{
	struct xfs_mount	*mp = rd->sc->mp;

	/* No pointers to ourselves or to garbage. */
	if (ino == rd->sc->ip->i_ino)
		return false;
	if (!xfs_verify_dir_ino(mp, ino))
		return false;

	/* No weird looking names or dot entries. */
	if (namelen > MAXNAMELEN || namelen <= 0)
		return false;
	if (namelen == 1 && name[0] == '.')
		return false;

	return true;
}

/* Allocate an in-core record to hold entries while we rebuild the dir data. */
STATIC int
xrep_dir_salvage_entry(
	struct xrep_dir		*rd,
	unsigned char		*name,
	unsigned int		namelen,
	xfs_ino_t		ino)
{
	struct xrep_dir_key	key = {
		.ino		= ino,
	};
	struct xfs_inode	*ip;
	unsigned int		i;
	int			error = 0;

	if (xchk_should_terminate(rd->sc, &error))
		return error;

	/* Truncate the name to the first illegal character. */
	for (i = 0; i < namelen && name[i] != 0 && name[i] != '/'; i++);
	key.namelen = i;
	key.hash = xfs_da_hashname(name, key.namelen);

	trace_xrep_dir_salvage_entry(rd->sc->ip, name, key.namelen, ino);

	/* If this is a '..' entry, we can save it for later... */
	if (key.namelen == 2 && name[0] == '.' && name[1] == '.') {
		switch (rd->parent_ino) {
		case NULLFSINO:
			/* Found a parent, save it for later. */
			rd->parent_ino = ino;
			break;
		default:
			/*
			 * Found more than one parent, so force a directory
			 * tree walk later.
			 */
			rd->parent_ino = 0;
			/* fall through */
		case 0:
			break;
		}
		return 0;
	}

	/*
	 * Compute the ftype or dump the entry if we can't.  We don't lock the
	 * inode because inodes can't change type while we have a reference.
	 */
	error = xfs_iget(rd->sc->mp, rd->sc->tp, ino, XFS_IGET_UNTRUSTED, 0,
			&ip);
	if (error)
		return 0;
	key.ftype = xfs_mode_to_ftype(VFS_I(ip)->i_mode);
	xfs_irele(ip);

	/* Remember this for later. */
	error = xblob_put(rd->dir_names, &key.name_cookie, name, key.namelen);
	if (error)
		return error;

	return xfbma_append(rd->dir_entries, &key);
}

/* Record a shortform directory entry for later reinsertion. */
STATIC int
xrep_dir_salvage_sf_entry(
	struct xrep_dir			*rd,
	struct xfs_dir2_sf_hdr		*sfp,
	struct xfs_dir2_sf_entry	*sfep)
{
	xfs_ino_t			ino;

	ino = xfs_dir2_sf_get_ino(rd->sc->mp, sfp, sfep);
	if (!xrep_dir_want_salvage(rd, sfep->name, sfep->namelen, ino))
		return 0;

	return xrep_dir_salvage_entry(rd, sfep->name, sfep->namelen, ino);
}

/* Record a regular directory entry for later reinsertion. */
STATIC int
xrep_dir_salvage_data_entry(
	struct xrep_dir			*rd,
	struct xfs_dir2_data_entry	*dep)
{
	xfs_ino_t			ino;

	ino = be64_to_cpu(dep->inumber);
	if (!xrep_dir_want_salvage(rd, dep->name, dep->namelen, ino))
		return 0;

	return xrep_dir_salvage_entry(rd, dep->name, dep->namelen, ino);
}

/* Try to recover block/data format directory entries. */
STATIC int
xrep_dir_recover_data(
	struct xrep_dir		*rd,
	struct xfs_buf		*bp)
{
	struct xfs_da_geometry	*geo = rd->sc->mp->m_dir_geo;
	unsigned int		offset;
	unsigned int		end;
	int			error;		/* error return value */

	/*
	 * Loop over the data portion of the block.
	 * Each object is a real entry (dep) or an unused one (dup).
	 */
	offset = geo->data_entry_offset;
	end = min_t(unsigned int, BBTOB(bp->b_length),
			xfs_dir3_data_end_offset(geo, bp->b_addr));

	while (offset < end) {
		struct xfs_dir2_data_unused	*dup = bp->b_addr + offset;
		struct xfs_dir2_data_entry	*dep = bp->b_addr + offset;

		if (xchk_should_terminate(rd->sc, &error))
			break;

		/* Skip unused entries. */
		if (be16_to_cpu(dup->freetag) == XFS_DIR2_DATA_FREE_TAG) {
			offset += be16_to_cpu(dup->length);
			continue;
		}

		/* Don't walk off the end of the block. */
		offset += xfs_dir2_data_entsize(rd->sc->mp, dep->namelen);
		if (offset > end)
			break;

		/* Ok, let's save this entry. */
		error = xrep_dir_salvage_data_entry(rd, dep);
		if (error)
			return error;

	}

	return 0;
}

/* Try to recover shortform directory entries. */
STATIC int
xrep_dir_recover_sf(
	struct xrep_dir			*rd)
{
	struct xfs_dir2_sf_hdr		*sfp;
	struct xfs_dir2_sf_entry	*sfep;
	struct xfs_dir2_sf_entry	*next;
	struct xfs_ifork		*ifp;
	unsigned char			*end;
	int				error;

	ifp = XFS_IFORK_PTR(rd->sc->ip, XFS_DATA_FORK);
	sfp = (struct xfs_dir2_sf_hdr *)rd->sc->ip->i_df.if_u1.if_data;
	end = (unsigned char *)ifp->if_u1.if_data + ifp->if_bytes;

	rd->parent_ino = xfs_dir2_sf_get_parent_ino(sfp);

	sfep = xfs_dir2_sf_firstentry(sfp);
	while ((unsigned char *)sfep < end) {
		if (xchk_should_terminate(rd->sc, &error))
			break;

		next = xfs_dir2_sf_nextentry(rd->sc->mp, sfp, sfep);
		if ((unsigned char *)next > end)
			break;

		/* Ok, let's save this entry. */
		error = xrep_dir_salvage_sf_entry(rd, sfp, sfep);
		if (error)
			return error;

		sfep = next;
	}

	return 0;
}

/*
 * Try to figure out the format of this directory from the data fork mappings
 * and the directory size.  If we can be reasonably sure of format, we can be
 * more aggressive in salvaging directory entries.  On return, @magic_guess
 * will be set to DIR3_BLOCK_MAGIC if we think this is a "block format"
 * directory; DIR3_DATA_MAGIC if we think this is a "data format" directory,
 * and 0 if we can't tell.
 */
STATIC void
xrep_dir_guess_format(
	struct xrep_dir		*rd,
	__be32			*magic_guess)
{
	struct xfs_inode	*ip = rd->sc->ip;
	struct xfs_da_geometry	*geo = rd->sc->mp->m_dir_geo;
	xfs_fileoff_t		last;
	int			error;

	ASSERT(xfs_sb_version_hascrc(&ip->i_mount->m_sb));

	*magic_guess = 0;

	/*
	 * If there's a single directory block and the directory size is
	 * exactly one block, this has to be a single block format directory.
	 */
	error = xfs_bmap_last_offset(ip, &last, XFS_DATA_FORK);
	if (!error && XFS_FSB_TO_B(ip->i_mount, last) == geo->blksize &&
	    ip->i_d.di_size == geo->blksize) {
		*magic_guess = cpu_to_be32(XFS_DIR3_BLOCK_MAGIC);
		return;
	}

	/*
	 * If the last extent before the leaf offset matches the directory
	 * size and the directory size is larger than 1 block, this is a
	 * data format directory.
	 */
	last = geo->leafblk;
	error = xfs_bmap_last_before(rd->sc->tp, ip, &last, XFS_DATA_FORK);
	if (!error &&
	    XFS_FSB_TO_B(ip->i_mount, last) > geo->blksize &&
	    XFS_FSB_TO_B(ip->i_mount, last) == ip->i_d.di_size) {
		*magic_guess = cpu_to_be32(XFS_DIR3_DATA_MAGIC);
		return;
	}
}

/* Recover directory entries from a specific directory block. */
STATIC int
xrep_dir_recover_dirblock(
	struct xrep_dir		*rd,
	__be32			magic_guess,
	xfs_dablk_t		dabno)
{
	struct xfs_dir2_data_hdr *hdr;
	struct xfs_buf		*bp;
	__be32			oldmagic;
	int			error;

	/*
	 * Try to read buffer.  We invalidate them in the next step so we don't
	 * bother to set a buffer type or ops.
	 */
	error = xfs_da_read_buf(rd->sc->tp, rd->sc->ip, dabno,
			XFS_DABUF_MAP_HOLE_OK, &bp, XFS_DATA_FORK, NULL);
	if (error || !bp)
		return error;

	hdr = bp->b_addr;
	oldmagic = hdr->magic;

	trace_xrep_dir_recover_dirblock(rd->sc->ip, dabno,
			be32_to_cpu(hdr->magic), be32_to_cpu(magic_guess));

	/*
	 * If we're sure of the block's format, proceed with the salvage
	 * operation using the specified magic number.
	 */
	if (magic_guess) {
		hdr->magic = magic_guess;
		goto recover;
	}

	/*
	 * If we couldn't guess what type of directory this is, then we will
	 * only salvage entries from directory blocks that match the magic
	 * number and pass verifiers.
	 */
	switch (hdr->magic) {
	case cpu_to_be32(XFS_DIR2_BLOCK_MAGIC):
	case cpu_to_be32(XFS_DIR3_BLOCK_MAGIC):
		if (!xrep_buf_verify_struct(bp, &xfs_dir3_block_buf_ops))
			goto out;
		break;
	case cpu_to_be32(XFS_DIR2_DATA_MAGIC):
	case cpu_to_be32(XFS_DIR3_DATA_MAGIC):
		if (!xrep_buf_verify_struct(bp, &xfs_dir3_data_buf_ops))
			goto out;
		break;
	default:
		goto out;
	}

recover:
	error = xrep_dir_recover_data(rd, bp);

out:
	hdr->magic = oldmagic;
	xfs_trans_brelse(rd->sc->tp, bp);
	return error;
}

/* Extract as many directory entries as we can. */
STATIC int
xrep_dir_recover(
	struct xrep_dir		*rd)
{
	struct xfs_iext_cursor	icur;
	struct xfs_bmbt_irec	got;
	struct xfs_scrub	*sc = rd->sc;
	struct xfs_ifork	*ifp;
	struct xfs_da_geometry	*geo = sc->mp->m_dir_geo;
	xfs_dablk_t		dabno;
	__be32			magic_guess;
	int			error = 0;

	if (rd->sc->ip->i_d.di_format == XFS_DINODE_FMT_LOCAL)
		return xrep_dir_recover_sf(rd);

	xrep_dir_guess_format(rd, &magic_guess);

	/* Iterate each directory data block in the data fork. */
	ifp = XFS_IFORK_PTR(sc->ip, XFS_DATA_FORK);
	for_each_xfs_iext(ifp, &icur, &got) {
		/* Leaf blocks come after all data blocks, so cut off there. */
		xfs_trim_extent(&got, 0, geo->leafblk);
		if (got.br_blockcount == 0)
			continue;

		for (dabno = round_up(got.br_startoff, geo->fsbcount);
		     dabno < got.br_startoff + got.br_blockcount;
		     dabno += geo->fsbcount) {
			if (xchk_should_terminate(rd->sc, &error))
				return error;

			error = xrep_dir_recover_dirblock(rd, magic_guess,
					dabno);
			if (error)
				break;
		}
	}

	return error;
}

/* Invalidate a directory's blocks and unmap them. */
STATIC int
xrep_dir_reset_nonlocal(
	struct xfs_scrub	*sc,
	struct xfs_inode	*dp)
{
	struct xfs_iext_cursor	icur;
	struct xfs_bmbt_irec	got;
	struct xfs_ifork	*ifp = XFS_IFORK_PTR(dp, XFS_DATA_FORK);
	struct xfs_buf		*bp;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_da_geometry	*geo = mp->m_dir_geo;
	xfs_fileoff_t		off;

	/*
	 * Invalidate each directory block.  All directory blocks are of
	 * fsbcount length and alignment, so we only need to walk those same
	 * offsets.
	 *
	 * We use TRYLOCK here (recall that we hold the ILOCK of the directory
	 * inode) so that we skip any buffer that's locked on the assumption
	 * that we don't own that block.
	 */
	for_each_xfs_iext(ifp, &icur, &got) {
		for (off = round_up(got.br_startoff, geo->fsbcount);
		     off < got.br_startoff + got.br_blockcount;
		     off += geo->fsbcount) {
			xfs_fsblock_t	fsbno;

			fsbno = (off - got.br_startoff) + got.br_startblock;
			bp = xfs_buf_incore(mp->m_ddev_targp,
					XFS_FSB_TO_DADDR(mp, fsbno),
					XFS_FSB_TO_BB(mp, geo->fsbcount),
					XBF_TRYLOCK | XBF_SCAN_STALE);
			if (bp) {
				xfs_buf_stale(bp);
				xfs_buf_relse(bp);
			}
		}
	}

	/* Now free all the blocks. */
	return xfs_bunmapi_range(&sc->tp, dp, XFS_DATA_FORK, 0,
			XFS_MAX_FILEOFF, XFS_BMAPI_NODISCARD);
}

/*
 * Free all the directory blocks and reset the data fork.  The caller must
 * join the inode to the transaction.  This function returns with the inode
 * joined to a clean scrub transaction.
 */
STATIC int
xrep_dir_reset_fork(
	struct xfs_scrub	*sc,
	xfs_ino_t		parent_ino)
{
	struct xfs_ifork	*ifp;
	struct xfs_da_args	*args = sc->buf;
	int			error;

	ifp = XFS_IFORK_PTR(sc->ip, XFS_DATA_FORK);

	/* Unmap all the directory buffers. */
	if (xfs_ifork_has_extents(sc->ip, XFS_DATA_FORK)) {
		error = xrep_dir_reset_nonlocal(sc, sc->ip);
		if (error)
			return error;
	}

	trace_xrep_dir_reset_fork(sc->ip, parent_ino);

	/* Reset the data fork to an empty data fork. */
	xfs_ifork_reset(ifp);
	ifp->if_flags = XFS_IFINLINE;
	ifp->if_bytes = 0;
	sc->ip->i_d.di_size = 0;

	/* Reinitialize the short form directory. */
	set_nlink(VFS_I(sc->ip), 2);
	args->geo = sc->mp->m_dir_geo;
	args->dp = sc->ip;
	args->trans = sc->tp;
	error = xfs_dir2_sf_create(args, parent_ino);
	if (error)
		return error;

	return xrep_roll_trans(sc);
}

/* Compare two dir keys, sorting in hash order. */
static int
xrep_dir_key_cmp(
	const void			*a,
	const void			*b)
{
	const struct xrep_dir_key	*ap = a;
	const struct xrep_dir_key	*bp = b;

	if (ap->hash > bp->hash)
		return 1;
	else if (ap->hash < bp->hash)
		return -1;
	return 0;
}

/*
 * Find all the directory entries for this inode by scraping them out of the
 * directory leaf blocks by hand.  The caller must clean up the lists if
 * anything goes wrong.
 */
STATIC int
xrep_dir_find_entries(
	struct xrep_dir		*rd)
{
	struct xfs_inode	*ip = rd->sc->ip;
	struct xfs_ifork	*ifp;
	int			error;

	error = xrep_ino_dqattach(rd->sc);
	if (error)
		return error;

	/* Extent map should be loaded. */
	ifp = XFS_IFORK_PTR(ip, XFS_DATA_FORK);
	if (XFS_IFORK_FORMAT(ip, XFS_DATA_FORK) != XFS_DINODE_FMT_LOCAL &&
	    !(ifp->if_flags & XFS_IFEXTENTS)) {
		error = xfs_iread_extents(rd->sc->tp, ip, XFS_DATA_FORK);
		if (error)
			return error;
	}

	/* Read every directory entry and record them in memory. */
	return xrep_dir_recover(rd);
}

/* Insert one dir entry. */
STATIC int
xrep_dir_insert_rec(
	const void			*item,
	void				*priv)
{
	struct xfs_name			name;
	const struct xrep_dir_key	*key = item;
	struct xrep_dir			*rd = priv;
	struct xfs_trans		*tp;
	char				*namebuf = rd->sc->buf;
	struct xfs_mount		*mp = rd->sc->mp;
	uint				resblks;
	int				error;

	if (XFS_FORCED_SHUTDOWN(mp))
		return -EIO;

	/* The entry name is stored in the in-core buffer. */
	name.name = namebuf;

	error = xblob_get(rd->dir_names, key->name_cookie, namebuf,
			key->namelen);
	if (error)
		return error;

	error = xblob_free(rd->dir_names, key->name_cookie);
	if (error)
		return error;

	trace_xrep_dir_insert_rec(rd->sc->ip, namebuf, key->namelen, key->ino,
			key->ftype);

	error = xfs_qm_dqattach(rd->sc->ip);
	if (error)
		return error;

	resblks = XFS_LINK_SPACE_RES(mp, key->namelen);
	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_link, resblks, 0, 0, &tp);
	if (error == -ENOSPC) {
		resblks = 0;
		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_link, 0, 0, 0, &tp);
	}
	if (error)
		return error;

	xfs_ilock(rd->sc->ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, rd->sc->ip, XFS_ILOCK_EXCL);

	name.len = key->namelen;
	name.type = key->ftype;
	error = xfs_dir_createname(tp, rd->sc->ip, &name, key->ino, resblks);
	if (error)
		goto err;

	if (name.type == XFS_DIR3_FT_DIR)
		inc_nlink(VFS_I(rd->sc->ip));
	xfs_trans_log_inode(tp, rd->sc->ip, XFS_ILOG_CORE);
	return xfs_trans_commit(tp);

err:
	xfs_trans_cancel(tp);
	return error;
}

/*
 * Insert all the attributes that we collected.
 *
 * Commit the repair transaction and drop the ilock because the attribute
 * setting code needs to be able to allocate special transactions and take the
 * ilock on its own.  Some day we'll have deferred attribute setting, at which
 * point we'll be able to use that to replace the attributes atomically and
 * safely.
 */
STATIC int
xrep_dir_rebuild_tree(
	struct xrep_dir		*rd)
{
	int			error;

	/*
	 * Commit the existing transaction and drop the ILOCK so that we can
	 * use a series of small transactions to rebuild the directory.
	 */
	error = xfs_trans_commit(rd->sc->tp);
	rd->sc->tp = NULL;
	if (error)
		return error;

	xfs_iunlock(rd->sc->ip, XFS_ILOCK_EXCL);
	rd->sc->ilock_flags &= ~XFS_ILOCK_EXCL;

	/*
	 * Sort the entries hash to minimize dabtree splits when we rebuild the
	 * directory tree information.
	 */
	error = xfbma_sort(rd->dir_entries, xrep_dir_key_cmp);
	if (error)
		return error;

	/* Re-add every entry to the directory. */
	return xfbma_iter_del(rd->dir_entries, xrep_dir_insert_rec, rd);
}

/*
 * If this directory entry points to the directory we're rebuilding, then the
 * directory we're scanning is the parent.  Remember the parent.
 */
STATIC int
xrep_dir_absorb_parent(
	struct xfs_inode	*dp,
	struct xfs_name		*name,
	unsigned int		dtype,
	void			*data)
{
	struct xrep_dir		*rd = data;
	int			error = 0;

	/* Uhoh, more than one parent for a dir? */
	if (rd->parent_ino != NULLFSINO)
		return -EFSCORRUPTED;

	if (xchk_should_terminate(rd->sc, &error))
		return error;

	/* We found a potential parent; remember this. */
	rd->parent_ino = dp->i_ino;
	return 0;
}

/*
 * Make sure we return with a valid parent inode.
 *
 * If the directory salvaging step found a single '..' entry, check the
 * alleged parent for a dentry pointing to the directory.  If this succeds,
 * we're done.  Otherwise, scan the entire filesystem for a parent.
 */
STATIC int
xrep_dir_validate_parent(
	struct xrep_dir		*rd)
{
	struct xfs_scrub	*sc = rd->sc;
	struct xfs_inode	*parent;
	xfs_nlink_t		expected_nlink, nlink;
	int			error;

	/*
	 * If the directory salvage scan found no parent or found an obviously
	 * incorrect parent, try asking the dcache for the parent.
	 *
	 * If the dcache doesn't know about a parent or the parent seems
	 * obviously incorrect, jump to the filesystem scan.
	 *
	 * Otherwise, if the alleged parent seems plausible, scan the directory
	 * to make sure it really points to us.
	 */
	if (!xrep_parent_acceptable(sc, rd->parent_ino))
		rd->parent_ino = xrep_parent_check_dcache(sc->ip);
	if (!xrep_parent_acceptable(sc, rd->parent_ino))
		goto scan;

	/*
	 * Grab this parent inode.  Since we release the inode before we cancel
	 * the scrub transaction and don't know if releasing the inode will
	 * trigger eofblocks cleanup (which allocates what would be a nested
	 * transaction), we avoid DONTCACHE here.
	 */
	error = xfs_iget(sc->mp, sc->tp, rd->parent_ino, XFS_IGET_UNTRUSTED, 0,
			&parent);
	if (error)
		goto scan;
	if (!S_ISDIR(VFS_I(parent)->i_mode))
		goto rele_scan;

	/*
	 * We prefer to keep the inode locked while we lock and search its
	 * alleged parent for a forward reference.  If we can grab the iolock,
	 * validate the pointers and we're done.  We must use nowait here to
	 * avoid an ABBA deadlock on the parent and the child inodes.
	 */
	if (!xfs_ilock_nowait(parent, XFS_IOLOCK_SHARED))
		goto rele_scan;

	/*
	 * If we're an unlinked directory, the parent /won't/ have a link
	 * to us.  Otherwise, it should have one link.
	 */
	expected_nlink = VFS_I(sc->ip)->i_nlink == 0 ? 0 : 1;

	error = xchk_parent_count_parent_dentries(sc, parent, &nlink);
	if (error)
		goto unlock_rele_scan;

	/* The parent is an exact match, we're done. */
	if (nlink == expected_nlink) {
		xfs_iunlock(parent, XFS_IOLOCK_SHARED);
		xfs_irele(parent);
		return 0;
	}

unlock_rele_scan:
	xfs_iunlock(parent, XFS_IOLOCK_SHARED);
rele_scan:
	xfs_irele(parent);
scan:
	/*
	 * If we're an unlinked directory, the parent /won't/ have a link
	 * to us.  Set the parent directory to the root.
	 */
	if (VFS_I(rd->sc->ip)->i_nlink == 0) {
		rd->parent_ino = sc->mp->m_sb.sb_rootino;
		return 0;
	}

	/* Scan the entire directory tree for the directory's parent. */
	error = xrep_scan_for_parents(sc, sc->ip->i_ino,
			xrep_dir_absorb_parent, rd);
	if (error)
		return error;

	return rd->parent_ino == NULLFSINO ? -EFSCORRUPTED : 0;
}

/*
 * Repair the directory metadata.
 *
 * XXX: Directory entry buffers can be multiple fsblocks in size.  The buffer
 * cache in XFS can't handle aliased multiblock buffers, so this might
 * misbehave if the directory blocks are crosslinked with other filesystem
 * metadata.
 *
 * XXX: Is it necessary to check the dcache for this directory to make sure
 * that we always recreate every cached entry?
 */
int
xrep_dir(
	struct xfs_scrub	*sc)
{
	struct xrep_dir		rd = {
		.sc		= sc,
		.parent_ino	= NULLFSINO,
	};
	int			error;

	/* Set up some storage */
	rd.dir_entries = xfbma_init(sizeof(struct xrep_dir_key));
	if (IS_ERR(rd.dir_entries))
		return PTR_ERR(rd.dir_entries);
	rd.dir_names = xblob_init();
	if (IS_ERR(rd.dir_names)) {
		error = PTR_ERR(rd.dir_names);
		goto out_arr;
	}

	/*
	 * The directory scrubber might have dropped the ILOCK, so pick it up
	 * again.
	 */
	if (!(sc->ilock_flags & XFS_ILOCK_EXCL)) {
		xfs_ilock(sc->ip, XFS_ILOCK_EXCL);
		sc->ilock_flags |= XFS_ILOCK_EXCL;
	}

	/* Collect directory entries by parsing raw leaf blocks. */
	error = xrep_dir_find_entries(&rd);
	if (error)
		goto out;

	/*
	 * Validate the parent pointer that we observed while salvaging the
	 * directory; or scan the filesystem to find one.  We drop the ILOCK
	 * on the directory being repaired to avoid ABBA deadlocks, though we
	 * maintain the directory IOLOCK to prevent concurrent modifications.
	 */
	xfs_iunlock(sc->ip, XFS_ILOCK_EXCL);
	error = xrep_dir_validate_parent(&rd);
	xfs_ilock(sc->ip, XFS_ILOCK_EXCL);
	if (error)
		goto out;

	/*
	 * Invalidate and truncate all data fork extents.  This is the point at
	 * which we are no longer able to bail out gracefully.  We commit the
	 * transaction here because the rebuilding step allocates its own
	 * transactions.
	 */
	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	error = xrep_dir_reset_fork(sc, rd.parent_ino);
	if (error)
		goto out;

	/* Now rebuild the directory information. */
	error = xrep_dir_rebuild_tree(&rd);
out:
	xblob_destroy(rd.dir_names);
out_arr:
	xfbma_destroy(rd.dir_entries);
	return error;
}
