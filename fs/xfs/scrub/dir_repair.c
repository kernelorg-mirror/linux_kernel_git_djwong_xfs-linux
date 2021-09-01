// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
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
#include "xfs_bmap_util.h"
#include "xfs_swapext.h"
#include "xfs_xchgrange.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/tempfile.h"
#include "scrub/xfarray.h"
#include "scrub/xfblob.h"
#include "scrub/parent.h"
#include "scrub/orphanage.h"

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
struct xrep_directory_key {
	/* Cookie for retrieval of the dirent name. */
	xfblob_cookie		name_cookie;

	/* Target inode number. */
	xfs_ino_t		ino;

	/* Hash of the dirent name. */
	unsigned int		hash;

	/* Length of the dirent name. */
	uint8_t			namelen;

	/* File type of the dirent. */
	uint8_t			ftype;
};

struct xrep_dir {
	struct xfs_scrub	*sc;

	/* Fixed-size array of xrep_directory_key structures. */
	struct xfarray		*dir_entries;

	/* Blobs containing directory entry names. */
	struct xfblob		*dir_names;

	/*
	 * This is the parent that we're going to set on the reconstructed
	 * directory.
	 */
	xfs_ino_t		parent_ino;

	/* nlink value of the corrected directory. */
	xfs_nlink_t		new_nlink;
};

/* Absorb up to 8 pages of dirents before we flush them to the temp dir. */
#define XREP_DIR_SALVAGE_BYTES	(PAGE_SIZE * 8)

static inline struct xfs_da_args *
xrep_directory_da_args(
	struct xfs_scrub	*sc)
{
	return sc->buf;
}

static inline unsigned char *
xrep_directory_namebuf(
	struct xfs_scrub	*sc)
{
	return sc->buf;
}

static inline struct xrep_orphanage_req *
xrep_dir_orphanage_req(
	struct xfs_scrub	*sc)
{
	return sc->buf + MAXNAMELEN + 1;
}

/* Set up for a directory repair. */
int
xrep_setup_directory(
	struct xfs_scrub	*sc)
{
	unsigned int		sz;
	int			error;

	error = xrep_orphanage_try_create(sc);
	if (error)
		return error;

	error = xrep_tempfile_create(sc, S_IFDIR);
	if (error)
		return error;

	/*
	 * We need a buffer to hold a directory entry name while we're building
	 * the new directory, later for the da state when we're freeing the old
	 * directory blocks, and a request to move the directory to the
	 * orphanage.  We don't need all three uses at the same time.
	 */
	sz = max_t(unsigned int, xrep_orphanage_req_sizeof(),
			sizeof(struct xfs_da_args));
	sc->buf = kvmalloc(sz,
			GFP_KERNEL | __GFP_NOWARN | __GFP_RETRY_MAYFAIL);
	if (!sc->buf)
		return -ENOMEM;

	return 0;
}

/*
 * Decide if we want to salvage this entry.  We don't bother with oversized
 * names or the dot entry.
 */
STATIC int
xrep_directory_want_salvage(
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
xrep_directory_salvage_entry(
	struct xrep_dir		*rd,
	unsigned char		*name,
	unsigned int		namelen,
	xfs_ino_t		ino)
{
	struct xrep_directory_key	key = {
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

	trace_xrep_directory_salvage_entry(rd->sc->ip, name, key.namelen, ino);

	/* Ignore '..' entries; we already picked the new parent. */
	if (key.namelen == 2 && name[0] == '.' && name[1] == '.') {
		trace_xrep_directory_salvaged_parent(rd->sc->ip, ino);
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
	error = xfblob_store(rd->dir_names, &key.name_cookie, name,
			key.namelen);
	if (error)
		return error;

	return xfarray_append(rd->dir_entries, &key);
}

/* Record a shortform directory entry for later reinsertion. */
STATIC int
xrep_directory_salvage_sf_entry(
	struct xrep_dir			*rd,
	struct xfs_dir2_sf_hdr		*sfp,
	struct xfs_dir2_sf_entry	*sfep)
{
	xfs_ino_t			ino;

	ino = xfs_dir2_sf_get_ino(rd->sc->mp, sfp, sfep);
	if (!xrep_directory_want_salvage(rd, sfep->name, sfep->namelen, ino))
		return 0;

	return xrep_directory_salvage_entry(rd, sfep->name, sfep->namelen, ino);
}

/* Record a regular directory entry for later reinsertion. */
STATIC int
xrep_directory_salvage_data_entry(
	struct xrep_dir			*rd,
	struct xfs_dir2_data_entry	*dep)
{
	xfs_ino_t			ino;

	ino = be64_to_cpu(dep->inumber);
	if (!xrep_directory_want_salvage(rd, dep->name, dep->namelen, ino))
		return 0;

	return xrep_directory_salvage_entry(rd, dep->name, dep->namelen, ino);
}

/* Try to recover block/data format directory entries. */
STATIC int
xrep_directory_recover_data(
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
		error = xrep_directory_salvage_data_entry(rd, dep);
		if (error)
			return error;

	}

	return 0;
}

/* Try to recover shortform directory entries. */
STATIC int
xrep_directory_recover_sf(
	struct xrep_dir			*rd)
{
	struct xfs_dir2_sf_hdr		*sfp;
	struct xfs_dir2_sf_entry	*sfep;
	struct xfs_dir2_sf_entry	*next;
	struct xfs_ifork		*ifp;
	xfs_ino_t			ino;
	unsigned char			*end;
	int				error;

	ifp = XFS_IFORK_PTR(rd->sc->ip, XFS_DATA_FORK);
	sfp = (struct xfs_dir2_sf_hdr *)rd->sc->ip->i_df.if_u1.if_data;
	end = (unsigned char *)ifp->if_u1.if_data + ifp->if_bytes;

	ino = xfs_dir2_sf_get_parent_ino(sfp);
	trace_xrep_directory_salvaged_parent(rd->sc->ip, ino);

	sfep = xfs_dir2_sf_firstentry(sfp);
	while ((unsigned char *)sfep < end) {
		if (xchk_should_terminate(rd->sc, &error))
			break;

		next = xfs_dir2_sf_nextentry(rd->sc->mp, sfp, sfep);
		if ((unsigned char *)next > end)
			break;

		/* Ok, let's save this entry. */
		error = xrep_directory_salvage_sf_entry(rd, sfp, sfep);
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
xrep_directory_guess_format(
	struct xrep_dir		*rd,
	__be32			*magic_guess)
{
	struct xfs_inode	*ip = rd->sc->ip;
	struct xfs_da_geometry	*geo = rd->sc->mp->m_dir_geo;
	xfs_fileoff_t		last;
	int			error;

	ASSERT(xfs_has_crc(ip->i_mount));

	*magic_guess = 0;

	/*
	 * If there's a single directory block and the directory size is
	 * exactly one block, this has to be a single block format directory.
	 */
	error = xfs_bmap_last_offset(ip, &last, XFS_DATA_FORK);
	if (!error && XFS_FSB_TO_B(ip->i_mount, last) == geo->blksize &&
	    ip->i_disk_size == geo->blksize) {
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
	    XFS_FSB_TO_B(ip->i_mount, last) == ip->i_disk_size) {
		*magic_guess = cpu_to_be32(XFS_DIR3_DATA_MAGIC);
		return;
	}
}

/* Recover directory entries from a specific directory block. */
STATIC int
xrep_directory_recover_dirblock(
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

	trace_xrep_directory_recover_dirblock(rd->sc->ip, dabno,
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
	error = xrep_directory_recover_data(rd, bp);

out:
	hdr->magic = oldmagic;
	xfs_trans_brelse(rd->sc->tp, bp);
	return error;
}

/* Insert one dir entry without cycling locks or transactions. */
STATIC int
xrep_directory_insert_rec(
	struct xrep_dir			*rd,
	const struct xrep_directory_key	*key)
{
	struct xfs_name			name = {
		.len			= key->namelen,
		.type			= key->ftype,
	};
	char				*namebuf;
	struct xfs_mount		*mp = rd->sc->mp;
	uint				resblks;
	int				error;

	name.name = namebuf = xrep_directory_namebuf(rd->sc);

	/*
	 * We want to use a separate transaction for each dirent that we're
	 * adding to the temporary directory.  However, directory salvaging
	 * uses the scrub transaction to avoid livelocking on directory
	 * tree loops, so we have to commit the existing scrub transaction to
	 * get it out of the way.
	 */
	error = xrep_trans_commit(rd->sc);
	if (error)
		return error;

	/* The entry name is stored in the in-core buffer. */
	error = xfblob_load(rd->dir_names, key->name_cookie, namebuf,
			key->namelen);
	if (error)
		return error;

	trace_xrep_directory_insert_rec(rd->sc->tempip, &name, key->ino);

	error = xfs_qm_dqattach(rd->sc->tempip);
	if (error)
		return error;

	resblks = XFS_LINK_SPACE_RES(mp, key->namelen);
	error = xchk_trans_alloc(rd->sc, resblks);
	if (error)
		return error;

	/*
	 * Join both inodes to the transaction.  We previously took the ILOCK
	 * of both inodes, and we intend to hang on to them no matter what
	 * happens here.
	 */
	xfs_trans_ijoin(rd->sc->tp, rd->sc->ip, 0);
	xfs_trans_ijoin(rd->sc->tp, rd->sc->tempip, 0);

	error = xfs_dir_createname(rd->sc->tp, rd->sc->tempip, &name, key->ino,
			resblks);
	if (error)
		return error;

	if (name.type == XFS_DIR3_FT_DIR)
		rd->new_nlink++;

	/* Roll both inodes, which takes care of logging both inode cores. */
	return xrep_roll_trans(rd->sc);
}

/*
 * Periodically flush salvaged directory entries to the temporary file.  This
 * is done to reduce the memory requirements of the directory rebuild, since
 * directories can contain up to 32GB of directory data.
 */
STATIC int
xrep_directory_flush_salvaged(
	struct xrep_dir		*rd)
{
	struct xrep_directory_key	key;
	uint64_t		nr = 0;
	int			error;

	/* Add all the salvaged entries to the temporary directory. */
	while (!(error = xfarray_load_next(rd->dir_entries, &nr, &key))) {
		error = xrep_directory_insert_rec(rd, &key);
		if (error)
			return error;
	}
	if (error && error != -ENODATA)
		return error;

	/* Empty out both arrays now that we've added the entries. */
	xfarray_truncate(rd->dir_entries);
	xfblob_truncate(rd->dir_names);
	return 0;
}

/* Extract as many directory entries as we can. */
STATIC int
xrep_directory_recover(
	struct xrep_dir		*rd)
{
	struct xfs_bmbt_irec	got;
	struct xfs_scrub	*sc = rd->sc;
	struct xfs_da_geometry	*geo = sc->mp->m_dir_geo;
	xfs_fileoff_t		offset;
	xfs_dablk_t		dabno;
	__be32			magic_guess;
	int			nmap;
	int			error;

	xrep_directory_guess_format(rd, &magic_guess);

	/* Iterate each directory data block in the data fork. */
	for (offset = 0;
	     offset < geo->leafblk;
	     offset = got.br_startoff + got.br_blockcount) {
		nmap = 1;
		error = xfs_bmapi_read(sc->ip, offset, geo->leafblk - offset,
				&got, &nmap, 0);
		if (error)
			return error;
		if (nmap != 1)
			return -EFSCORRUPTED;
		if (!xfs_bmap_is_written_extent(&got))
			continue;

		for (dabno = round_up(got.br_startoff, geo->fsbcount);
		     dabno < got.br_startoff + got.br_blockcount;
		     dabno += geo->fsbcount) {
			if (xchk_should_terminate(rd->sc, &error))
				return error;

			error = xrep_directory_recover_dirblock(rd,
					magic_guess, dabno);
			if (error)
				return error;

			/* Flush dirents to constrain memory usage. */
			if (xfarray_bytes(rd->dir_entries) +
			    xfblob_bytes(rd->dir_names) <
			    XREP_DIR_SALVAGE_BYTES)
				continue;

			error = xrep_directory_flush_salvaged(rd);
			if (error)
				return error;
		}
	}

	return 0;
}

/*
 * Find all the directory entries for this inode by scraping them out of the
 * directory leaf blocks by hand, and flushing them into the temp dir.
 */
STATIC int
xrep_directory_find_entries(
	struct xrep_dir		*rd)
{
	struct xfs_inode	*ip = rd->sc->ip;
	int			error;

	error = xrep_ino_dqattach(rd->sc);
	if (error)
		return error;

	/* Salvage directory entries from the old directory. */
	if (ip->i_df.if_format == XFS_DINODE_FMT_LOCAL) {
		error = xrep_directory_recover_sf(rd);
	} else {
		error = xfs_iread_extents(rd->sc->tp, ip, XFS_DATA_FORK);
		if (error)
			return error;

		error = xrep_directory_recover(rd);
	}
	if (error)
		return error;

	return xrep_directory_flush_salvaged(rd);
}

/* Invalidate a directory's blocks and unmap them. */
STATIC int
xrep_directory_reset_nonlocal(
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
					XBF_TRYLOCK | _XBF_IGNORE_STALE);
			if (bp) {
				xfs_buf_stale(bp);
				xfs_buf_relse(bp);
			}
		}
	}

	/* Free all the old dir blocks; don't discard them for speed. */
	return xfs_bunmapi_range(&sc->tp, dp, XFS_BMAPI_NODISCARD, 0,
			XFS_MAX_FILEOFF);
}

/*
 * Free all the directory blocks and reset the data fork.  The caller must
 * join the inode to the transaction.  This function returns with the inode
 * joined to a clean scrub transaction.
 */
STATIC int
xrep_directory_reset_fork(
	struct xfs_scrub	*sc,
	xfs_ino_t		parent_ino)
{
	struct xfs_ifork	*ifp = XFS_IFORK_PTR(sc->tempip, XFS_DATA_FORK);
	struct xfs_da_args	*args = xrep_directory_da_args(sc);
	int			error;

	/* Unmap all the directory buffers. */
	if (xfs_ifork_has_extents(ifp)) {
		error = xrep_directory_reset_nonlocal(sc, sc->tempip);
		if (error)
			return error;
	}

	trace_xrep_directory_reset_fork(sc->tempip, parent_ino);

	/* Reset the data fork to an empty data fork. */
	xfs_idestroy_fork(ifp);
	ifp->if_bytes = 0;
	sc->tempip->i_disk_size = 0;

	/* Reinitialize the short form directory. */
	args->geo = sc->mp->m_dir_geo;
	args->dp = sc->tempip;
	args->trans = sc->tp;
	error = xfs_dir2_sf_create(args, parent_ino);
	if (error)
		return error;

	return xrep_roll_trans(sc);
}

/*
 * Prepare both inodes' directory forks for extent swapping.  Promote the
 * tempfile from short format to leaf format, and if the file being repaired
 * has a short format attr fork, turn it into an empty extent list.
 */
STATIC int
xrep_directory_swap_prep(
	struct xfs_scrub	*sc,
	bool			temp_local,
	bool			ip_local)
{
	int			error;

	/*
	 * If the tempfile's attributes are in shortform format, convert that
	 * to a single leaf extent so that we can use the atomic extent swap.
	 */
	if (temp_local) {
		struct xfs_da_args	args = {
			.dp		= sc->tempip,
			.geo		= sc->mp->m_dir_geo,
			.whichfork	= XFS_DATA_FORK,
			.trans		= sc->tp,
			.total		= 1,
		};

		error = xfs_dir2_sf_to_block(&args);
		if (error)
			return error;

		error = xfs_defer_finish(&sc->tp);
		if (error)
			return error;
	}

	/*
	 * If the file being repaired had a shortform attribute fork, convert
	 * that to an empty extent list in preparation for the atomic extent
	 * swap.
	 */
	if (ip_local) {
		struct xfs_ifork	*ifp;

		ifp = XFS_IFORK_PTR(sc->ip, XFS_DATA_FORK);
		xfs_idestroy_fork(ifp);
		ifp->if_format = XFS_DINODE_FMT_EXTENTS;
		ifp->if_nextents = 0;
		ifp->if_bytes = 0;
		ifp->if_u1.if_root = NULL;
		ifp->if_height = 0;

		xfs_trans_log_inode(sc->tp, sc->ip,
				XFS_ILOG_CORE | XFS_ILOG_DDATA);
	}

	return 0;
}

/*
 * Set the owner for this directory block to the directory being repaired.
 * Return the magic number that we found, or the usual negative error.
 */
STATIC int
xrep_directory_reset_owner(
	struct xfs_scrub		*sc,
	xfs_dablk_t			dabno,
	struct xfs_buf			*bp,
	unsigned int			*magic)
{
	struct xfs_da_geometry		*geo = sc->mp->m_dir_geo;
	struct xfs_dir3_data_hdr	*data3 = bp->b_addr;
	struct xfs_da3_blkinfo		*info3 = bp->b_addr;
	struct xfs_dir3_free_hdr	*free3 = bp->b_addr;
	struct xfs_dir2_data_entry	*dep;

	/* Directory data blocks. */
	if (dabno < geo->leafblk) {
		*magic = be32_to_cpu(data3->hdr.magic);
		if (*magic != XFS_DIR3_BLOCK_MAGIC &&
		    *magic != XFS_DIR3_DATA_MAGIC)
			return -EFSCORRUPTED;

		/*
		 * If this is a block format directory, it's possible that the
		 * block was created as part of converting the temp directory
		 * from short format to block format in order to use the atomic
		 * extent swap.  In that case, the '.' entry will be set to
		 * the temp dir, so find the dot entry and reset it.
		 */
		if (*magic == XFS_DIR3_BLOCK_MAGIC) {
			dep = bp->b_addr + geo->data_entry_offset;
			if (dep->namelen != 1 || dep->name[0] != '.')
				return -EFSCORRUPTED;

			dep->inumber = cpu_to_be64(sc->ip->i_ino);
		}

		data3->hdr.owner = cpu_to_be64(sc->ip->i_ino);
		return 0;
	}

	/* Directory leaf and da node blocks. */
	if (dabno < geo->freeblk) {
		*magic = be16_to_cpu(info3->hdr.magic);
		switch (*magic) {
		case XFS_DA3_NODE_MAGIC:
		case XFS_DIR3_LEAF1_MAGIC:
		case XFS_DIR3_LEAFN_MAGIC:
			break;
		default:
			return -EFSCORRUPTED;
		}

		info3->owner = cpu_to_be64(sc->ip->i_ino);
		return 0;
	}

	/* Directory free blocks. */
	*magic = be32_to_cpu(free3->hdr.magic);
	if (*magic != XFS_DIR3_FREE_MAGIC)
		return -EFSCORRUPTED;

	free3->hdr.owner = cpu_to_be64(sc->ip->i_ino);
	return 0;
}

/*
 * If the buffer didn't have buffer ops set, we need to set them now that we've
 * dirtied the directory block.
 */
STATIC void
xrep_directory_set_verifier(
	unsigned int		magic,
	struct xfs_buf		*bp)
{
	switch (magic) {
	case XFS_DIR3_BLOCK_MAGIC:
		bp->b_ops = &xfs_dir3_block_buf_ops;
		break;
	case XFS_DIR3_DATA_MAGIC:
		bp->b_ops = &xfs_dir3_data_buf_ops;
		break;
	case XFS_DA3_NODE_MAGIC:
		bp->b_ops = &xfs_da3_node_buf_ops;
		break;
	case XFS_DIR3_LEAF1_MAGIC:
		bp->b_ops = &xfs_dir3_leaf1_buf_ops;
		break;
	case XFS_DIR3_LEAFN_MAGIC:
		bp->b_ops = &xfs_dir3_leafn_buf_ops;
		break;
	case XFS_DIR3_FREE_MAGIC:
		bp->b_ops = &xfs_dir3_free_buf_ops;
		break;
	}

	xfs_buf_set_ref(bp, XFS_DIR_BTREE_REF);
}

/*
 * Change the owner field of every block in the data fork to match the
 * directory being repaired.
 */
STATIC int
xrep_directory_swap_owner(
	struct xfs_scrub		*sc)
{
	struct xfs_bmbt_irec		map;
	struct xfs_da_geometry		*geo = sc->mp->m_dir_geo;
	struct xfs_buf			*bp;
	xfs_fileoff_t			offset = 0;
	xfs_fileoff_t			end = XFS_MAX_FILEOFF;
	xfs_dablk_t			dabno;
	int				nmap;
	int				error;

	for (offset = 0;
	     offset < end;
	     offset = map.br_startoff + map.br_blockcount) {
		nmap = 1;
		error = xfs_bmapi_read(sc->tempip, offset, end - offset,
				&map, &nmap, 0);
		if (error)
			return error;
		if (nmap != 1)
			return -EFSCORRUPTED;
		if (!xfs_bmap_is_written_extent(&map))
			continue;


		for (dabno = round_up(map.br_startoff, geo->fsbcount);
		     dabno < map.br_startoff + map.br_blockcount;
		     dabno += geo->fsbcount) {
			unsigned int	magic;

			error = xfs_da_read_buf(sc->tp, sc->tempip,
					dabno, 0, &bp, XFS_DATA_FORK, NULL);
			if (error)
				return error;
			if (!bp)
				return -EFSCORRUPTED;

			error = xrep_directory_reset_owner(sc, dabno, bp,
					&magic);
			if (error) {
				xfs_trans_brelse(sc->tp, bp);
				return error;
			}

			if (bp->b_ops == NULL)
				xrep_directory_set_verifier(magic, bp);

			xfs_trans_ordered_buf(sc->tp, bp);
			xfs_trans_brelse(sc->tp, bp);
		}
	}

	return 0;
}

/*
 * If both files' directory structure are in short format, we can copy
 * the short format data from the tempfile to the repaired file if it'll
 * fit.
 */
STATIC void
xrep_directory_swap_local(
	struct xfs_scrub	*sc,
	int			newsize)
{
	struct xfs_ifork	*ifp1, *ifp2;

	ifp1 = XFS_IFORK_PTR(sc->tempip, XFS_DATA_FORK);
	ifp2 = XFS_IFORK_PTR(sc->ip, XFS_DATA_FORK);

	xfs_idata_realloc(sc->ip, ifp2->if_bytes - ifp1->if_bytes,
			XFS_DATA_FORK);

	memcpy(ifp2->if_u1.if_data, ifp1->if_u1.if_data, newsize);
	xfs_trans_log_inode(sc->tp, sc->ip, XFS_ILOG_CORE | XFS_ILOG_DDATA);
}

static struct xfs_name xfs_name_dot = {
	.name	= (unsigned char *)".",
	.len	= 1,
	.type	= XFS_DIR3_FT_DIR,
};

/* Swap the temporary directory's data fork with the one being repaired. */
STATIC int
xrep_directory_swap(
	struct xrep_dir		*rd)
{
	struct xfs_swapext_req	req;
	struct xfs_swapext_res	res;
	struct xfs_scrub	*sc = rd->sc;
	bool			ip_local, temp_local;
	int			error;

	error = xrep_tempfile_swapext_prep(sc, XFS_DATA_FORK, &req, &res);
	if (error)
		return error;

	error = xchk_trans_alloc(sc, res.resblks);
	if (error)
		return error;

	sc->temp_ilock_flags |= XFS_ILOCK_EXCL;
	sc->ilock_flags |= XFS_ILOCK_EXCL;
	xfs_xchg_range_ilock(sc->tp, sc->ip, sc->tempip);

	/*
	 * Reset the temporary directory's '.' entry to point to the directory
	 * we're repairing.  Note: shortform directories lack the dot entry.
	 *
	 * It's possible that this replacement could also expand a sf tempdir
	 * into block format.
	 */
	if (sc->tempip->i_df.if_format != XFS_DINODE_FMT_LOCAL) {
		error = xfs_dir_replace(sc->tp, sc->tempip, &xfs_name_dot,
				sc->ip->i_ino, res.resblks);
		if (error)
			return error;
	}

	/*
	 * Reset the temporary directory's '..' entry to point to the parent
	 * that we found.  The temporary directory was created with the root
	 * directory as the parent, so we can skip this if repairing a
	 * subdirectory of the root.
	 *
	 * It's also possible that this replacement could also expand a sf
	 * tempdir into block format.
	 */
	if (rd->parent_ino != sc->mp->m_rootip->i_ino) {
		error = xfs_dir_replace(sc->tp, rd->sc->tempip,
				&xfs_name_dotdot, rd->parent_ino, res.resblks);
		if (error)
			return error;
	}

	/*
	 * Changing the dot and dotdot entries could have changed the shape of
	 * the directory, so we recompute these.
	 */
	ip_local = sc->ip->i_df.if_format == XFS_DINODE_FMT_LOCAL;
	temp_local = sc->tempip->i_df.if_format == XFS_DINODE_FMT_LOCAL;

	/*
	 * If the both files have a local format data fork and the rebuilt
	 * directory data would fit in the repaired file's data fork, copy
	 * the contents from the tempfile and declare ourselves done.
	 */
	if (ip_local && temp_local) {
		if (sc->tempip->i_disk_size <= XFS_IFORK_DSIZE(sc->ip)) {
			xrep_directory_swap_local(sc, sc->tempip->i_disk_size);
			set_nlink(VFS_I(sc->ip), rd->new_nlink);
			return 0;
		}
	}

	/* Clean the transaction before we start working on the extent swap. */
	error = xrep_roll_trans(rd->sc);
	if (error)
		return error;

	/* Otherwise, make sure both data forks are in block-mapping mode. */
	error = xrep_directory_swap_prep(sc, temp_local, ip_local);
	if (error)
		return error;

	/* Rewrite the owner field of all attr blocks in the temporary file. */
	error = xrep_directory_swap_owner(sc);
	if (error)
		return error;

	/*
	 * Set nlink of the directory under repair to the number of
	 * subdirectories that will be in the new directory data.  Do this in
	 * the same transaction sequence that (atomically) commits the new
	 * data.
	 */
	set_nlink(VFS_I(sc->ip), rd->new_nlink);

	return xfs_swapext(&sc->tp, &req);
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
xrep_directory_rebuild_tree(
	struct xrep_dir		*rd)
{
	int			error;

	trace_xrep_directory_rebuild_tree(rd->sc->ip, rd->parent_ino);

	/*
	 * Swap the tempdir's data fork with the file being repaired.  This
	 * recreates the transaction and re-takes the ILOCK in the scrub
	 * context.
	 */
	error = xrep_directory_swap(rd);
	if (error)
		return error;

	/*
	 * Release the old directory blocks and reset the data fork of the temp
	 * directory to an empty shortform directory because inactivation does
	 * nothing for directories.
	 */
	return xrep_directory_reset_fork(rd->sc, rd->sc->mp->m_rootip->i_ino);
}

/*
 * Try to find the parent of the directory being repaired.
 *
 * If this is a directory tree root or an unlinked directory, we already know
 * the answer.  Otherwise, try to look it up in the directory; if we get a
 * reasonable value, we'll use that.
 *
 * NOTE: This function will someday be augmented by the directory parent repair
 * code, which will know how to check the parent and scan the filesystem if
 * we cannot find anything.  Inode scans will have to be done before we start
 * salvaging directory entries, so we do this now.
 */
STATIC int
xrep_directory_find_parent(
	struct xrep_dir		*rd,
	bool			*move_orphanage)
{
	struct xfs_scrub	*sc = rd->sc;
	xfs_ino_t		parent_ino;
	int			error;

	/*
	 * If we're the root directory, we are our own parent.  If we're an
	 * unlinked directory, the parent /won't/ have a link to us.  Set the
	 * parent directory to the root for both cases.
	 */
	if (rd->sc->ip->i_ino == sc->mp->m_sb.sb_rootino ||
	    VFS_I(rd->sc->ip)->i_nlink == 0) {
		rd->parent_ino = sc->mp->m_sb.sb_rootino;
		return 0;
	}

	/* Does the VFS dcache have an answer for us? */
	parent_ino = xrep_parent_from_dcache(sc);
	error = xrep_parent_confirm(sc, &parent_ino);
	if (!error && parent_ino != NULLFSINO)
		goto foundit;

	/*
	 * Try to look up '..'; if it seems plausible, go with it.  Check that
	 * the parent directory actually points to this directory.  If so, we
	 * are good to go.  Any errors just push us to scanning the fs.
	 */
	parent_ino = xrep_dotdot_lookup(sc);
	error = xrep_parent_confirm(sc, &parent_ino);
	if (!error && parent_ino != NULLFSINO)
		goto foundit;

	/* Otherwise, scan the entire filesystem. */
	error = xrep_parent_scan(sc, &parent_ino);
	if (error)
		return error;
	if (parent_ino == NULLFSINO) {
		/*
		 * Temporarily assign the root dir as the parent; we'll move
		 * this to the orphanage after swapping the dir contents.
		 */
		*move_orphanage = true;
		rd->parent_ino = sc->mp->m_sb.sb_rootino;
		return 0;
	}

foundit:
	rd->parent_ino = parent_ino;
	return 0;
}

/*
 * Move the current file to the orphanage.  Caller must not hold any inode
 * locks.  Upon return, the scrub state will reflect the transaction, ijoin,
 * and inode lock states.
 */
STATIC int
xrep_dir_move_to_orphanage(
	struct xfs_scrub	*sc)
{
	struct xrep_orphanage_req *orph = xrep_dir_orphanage_req(sc);
	unsigned char		*namebuf = xrep_directory_namebuf(sc);
	int			error;

	/* No orphanage?  We can't fix this. */
	if (!sc->orphanage)
		return -EFSCORRUPTED;

	/*
	 * If we can take the orphanage's iolock then we're ready to move.
	 *
	 * If we can't, release the iolock on the child, and then try to iolock
	 * the orphanage and child at the same time.  Use trylock for the
	 * second lock so that we don't ABBA deadlock the system.
	 */
	if (!xrep_orphanage_ilock_nowait(sc, XFS_IOLOCK_EXCL)) {
		xfs_ino_t	orig_parent, new_parent;

		orig_parent = xrep_dotdot_lookup(sc);

		xchk_iunlock(sc, sc->ilock_flags);
		error = xrep_orphanage_iolock_two(sc);
		if (error)
			return error;

		/*
		 * If the parent changed or the child was unlinked while the
		 * child directory was unlocked, we don't need to move the
		 * child to the orphanage after all.
		 */
		new_parent = xrep_dotdot_lookup(sc);
		if (orig_parent != new_parent || VFS_I(sc->ip)->i_nlink == 0)
			return 0;
	}

	/*
	 * Move the directory to the orphanage, and let scrub teardown unlock
	 * everything for us.
	 */
	xrep_orphanage_compute_blkres(sc, orph);

	error = xrep_orphanage_compute_name(orph, namebuf);
	if (error)
		return error;

	error = xfs_trans_reserve_more(sc->tp,
			orph->orphanage_blkres + orph->child_blkres, 0);
	if (error)
		return error;

	error = xrep_orphanage_ilock_resv_quota(orph);
	if (error)
		return error;

	return xrep_orphanage_adopt(orph);
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
xrep_directory(
	struct xfs_scrub	*sc)
{
	struct xrep_dir		rd = {
		.sc		= sc,
		.parent_ino	= NULLFSINO,
		.new_nlink	= 2,
	};
	bool			move_orphanage = false;
	int			error;

	/* Set up some storage */
	rd.dir_entries = xfarray_create("dir entries",
			sizeof(struct xrep_directory_key));
	if (IS_ERR(rd.dir_entries))
		return PTR_ERR(rd.dir_entries);
	rd.dir_names = xfblob_create("dir names");
	if (IS_ERR(rd.dir_names)) {
		error = PTR_ERR(rd.dir_names);
		goto out_arr;
	}

	/*
	 * Drop the ILOCK and MMAPLOCK on this directory; we don't need to
	 * hold these to maintain control over the directory we're fixing.
	 * This should leave us holding only IOLOCK_EXCL.
	 */
	if (sc->ilock_flags & XFS_ILOCK_EXCL)
		xchk_iunlock(sc, XFS_ILOCK_EXCL);
	xchk_iunlock(sc, XFS_MMAPLOCK_EXCL);

	/* Figure out who is going to be the parent of this directory. */
	error = xrep_directory_find_parent(&rd, &move_orphanage);
	if (error)
		goto out_names;

	/*
	 * Grab ILOCK_EXCL on both the directory and the tempdir so that we can
	 * salvage directory entries into the tempdir.
	 */
	xrep_tempfile_ilock_two(sc, XFS_ILOCK_EXCL);

	/* Collect directory entries by parsing raw leaf blocks. */
	error = xrep_directory_find_entries(&rd);
	if (error)
		goto out_names;

	/*
	 * Now that we've stuffed all the salvaged dirents in the temporary
	 * dir, drop the in-memory staging areas, cancel the scrub transaction,
	 * and drop both ILOCKs.  The extent swap that we use to commit the new
	 * directory requires special transactions and locking.
	 */
	xfblob_destroy(rd.dir_names);
	xfarray_destroy(rd.dir_entries);
	rd.dir_names = NULL;
	rd.dir_entries = NULL;

	xchk_trans_cancel(sc);
	xchk_iunlock(sc, XFS_ILOCK_EXCL);
	xrep_tempfile_iunlock(sc, XFS_ILOCK_EXCL);

	error = xrep_directory_rebuild_tree(&rd);
	if (error || !move_orphanage)
		return error;

	/*
	 * We hold ILOCK_EXCL on both the directory and the tempdir after a
	 * successful rebuild.  Before we can move the directory to the
	 * orphanage, we must roll to a clean unjoined transaction and drop the
	 * ILOCKs on the dir and the temp dir.  We still hold IOLOCK_EXCL on
	 * the dir, so nobody will be able to access it in the mean time.
	 */
	error = xfs_trans_roll(&sc->tp);
	if (error)
		return error;

	xchk_iunlock(sc, XFS_ILOCK_EXCL);
	xrep_tempfile_iunlock(sc, XFS_ILOCK_EXCL);

	return xrep_dir_move_to_orphanage(sc);

out_names:
	xfblob_destroy(rd.dir_names);
out_arr:
	xfarray_destroy(rd.dir_entries);
	return error;
}
