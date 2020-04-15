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
#include "xfs_btree.h"
#include "xfs_bit.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_inode.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_dir2.h"
#include "xfs_attr.h"
#include "xfs_attr_leaf.h"
#include "xfs_attr_sf.h"
#include "xfs_attr_remote.h"
#include "xfs_bmap.h"
#include "xfs_bmap_util.h"
#include "xfs_swapext.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/array.h"
#include "scrub/blob.h"
#include "scrub/attr.h"

/*
 * Extended Attribute Repair
 * =========================
 *
 * We repair extended attributes by reading the attribute fork blocks looking
 * for keys and values, then truncate the entire attr fork and reinsert all
 * the attributes.  Unfortunately, there's no secondary copy of most extended
 * attribute data, which means that if we blow up midway through there's
 * little we can do.
 */

struct xrep_xattr_key {
	xblob_cookie		value_cookie;
	xblob_cookie		name_cookie;
	uint			hash;
	int			flags;
	uint32_t		valuelen;
	uint16_t		namelen;
} __packed;

struct xrep_xattr {
	struct xfs_scrub	*sc;
	struct xfbma		*xattr_records;
	struct xblob		*xattr_blobs;

	/* Size of the largest attribute value we're trying to salvage. */
	size_t			max_valuelen;
};

/*
 * Decide if we want to salvage this attribute.  We don't bother with
 * incomplete or oversized keys or values.
 */
STATIC int
xrep_xattr_want_salvage(
	int			flags,
	const void		*name,
	int			namelen,
	int			valuelen)
{
	if (flags & XFS_ATTR_INCOMPLETE)
		return false;
	if (namelen > XATTR_NAME_MAX || namelen <= 0)
		return false;
	if (valuelen > XATTR_SIZE_MAX || valuelen < 0)
		return false;
	if (!xfs_attr_namecheck(name, namelen))
		return false;
	return true;
}

/* Allocate an in-core record to hold xattrs while we rebuild the xattr data. */
STATIC int
xrep_xattr_salvage_key(
	struct xrep_xattr	*rx,
	int			flags,
	unsigned char		*name,
	int			namelen,
	unsigned char		*value,
	int			valuelen)
{
	struct xrep_xattr_key	key = {
		.valuelen	= valuelen,
		.flags		= flags & (XFS_ATTR_ROOT | XFS_ATTR_SECURE),
		.namelen	= namelen,
	};
	int			error = 0;

	if (xchk_should_terminate(rx->sc, &error))
		return error;

	trace_xrep_xattr_salvage_key(rx->sc->ip, key.flags, name, namelen,
			valuelen);

	error = xblob_put(rx->xattr_blobs, &key.name_cookie, name, namelen);
	if (error)
		return error;
	error = xblob_put(rx->xattr_blobs, &key.value_cookie, value, valuelen);
	if (error)
		return error;

	key.hash = xfs_da_hashname(name, namelen);

	error = xfbma_append(rx->xattr_records, &key);
	if (error)
		return error;

	rx->max_valuelen = max_t(size_t, rx->max_valuelen, valuelen);
	return 0;
}

/*
 * Record a shortform extended attribute key & value for later reinsertion
 * into the inode.
 */
STATIC int
xrep_xattr_salvage_sf_attr(
	struct xrep_xattr		*rx,
	struct xfs_attr_sf_entry	*sfe)
{
	unsigned char			*value = &sfe->nameval[sfe->namelen];

	if (!xrep_xattr_want_salvage(sfe->flags, sfe->nameval, sfe->namelen,
			sfe->valuelen))
		return 0;

	return xrep_xattr_salvage_key(rx, sfe->flags, sfe->nameval,
			sfe->namelen, value, sfe->valuelen);
}

/*
 * Record a local format extended attribute key & value for later reinsertion
 * into the inode.
 */
STATIC int
xrep_xattr_salvage_local_attr(
	struct xrep_xattr		*rx,
	struct xfs_attr_leaf_entry	*ent,
	unsigned int			nameidx,
	const char			*buf_end,
	struct xfs_attr_leaf_name_local	*lentry)
{
	unsigned char			*value;
	unsigned long			*usedmap = xchk_xattr_usedmap(rx->sc);
	unsigned int			valuelen;
	unsigned int			namesize;

	/*
	 * Decode the leaf local entry format.  If something seems wrong, we
	 * junk the attribute.
	 */
	valuelen = be16_to_cpu(lentry->valuelen);
	namesize = xfs_attr_leaf_entsize_local(lentry->namelen, valuelen);
	if ((char *)lentry + namesize > buf_end)
		return 0;
	if (!xrep_xattr_want_salvage(ent->flags, lentry->nameval,
			lentry->namelen, valuelen))
		return 0;
	if (!xchk_xattr_set_map(rx->sc, usedmap, nameidx, namesize))
		return 0;

	/* Try to save this attribute. */
	value = &lentry->nameval[lentry->namelen];
	return xrep_xattr_salvage_key(rx, ent->flags, lentry->nameval,
			lentry->namelen, value, valuelen);
}

/*
 * Record a remote format extended attribute key & value for later reinsertion
 * into the inode.
 */
STATIC int
xrep_xattr_salvage_remote_attr(
	struct xrep_xattr		*rx,
	struct xfs_attr_leaf_entry	*ent,
	unsigned int			nameidx,
	const char			*buf_end,
	struct xfs_attr_leaf_name_remote *rentry,
	unsigned int			ent_idx,
	struct xfs_buf			*leaf_bp)
{
	struct xfs_da_args		args = {
		.trans	= rx->sc->tp,
		.dp	= rx->sc->ip,
		.index	= ent_idx,
		.geo	= rx->sc->mp->m_attr_geo,
	};
	unsigned long			*usedmap = xchk_xattr_usedmap(rx->sc);
	unsigned char			*value;
	unsigned int			valuelen;
	unsigned int			namesize;
	int				error;

	/*
	 * Decode the leaf remote entry format.  If something seems wrong, we
	 * junk the attribute.  Note that we should never find a zero-length
	 * remote attribute value.
	 */
	valuelen = be32_to_cpu(rentry->valuelen);
	namesize = xfs_attr_leaf_entsize_remote(rentry->namelen);
	if ((char *)rentry + namesize > buf_end)
		return 0;
	if (valuelen == 0 ||
	    !xrep_xattr_want_salvage(ent->flags, rentry->name, rentry->namelen,
			valuelen))
		return 0;
	if (!xchk_xattr_set_map(rx->sc, usedmap, nameidx, namesize))
		return 0;

	/*
	 * Find somewhere to save this value.  We can't use the xchk_xattr_buf
	 * here because we're still using the memory for the attr block bitmap.
	 */
	value = kmem_alloc_large(valuelen, KM_MAYFAIL);
	if (!value)
		return -ENOMEM;

	/* Look up the remote value and stash it for reconstruction. */
	args.valuelen = valuelen;
	args.namelen = rentry->namelen;
	args.name = rentry->name;
	args.value = value;
	error = xfs_attr3_leaf_getvalue(leaf_bp, &args);
	if (error || args.rmtblkno == 0)
		goto err_free;

	error = xfs_attr_rmtval_get(&args);
	if (error)
		goto err_free;

	/* Try to save this attribute. */
	error = xrep_xattr_salvage_key(rx, ent->flags, rentry->name,
			rentry->namelen, value, valuelen);
err_free:
	/* remote value was garbage, junk it */
	if (error == -EFSBADCRC || error == -EFSCORRUPTED)
		error = 0;
	kmem_free(value);
	return error;
}

/* Extract every xattr key that we can from this attr fork block. */
STATIC int
xrep_xattr_recover_leaf(
	struct xrep_xattr		*rx,
	struct xfs_buf			*bp)
{
	struct xfs_attr3_icleaf_hdr	leafhdr;
	struct xfs_scrub		*sc = rx->sc;
	struct xfs_mount		*mp = sc->mp;
	struct xfs_attr_leafblock	*leaf;
	unsigned long			*usedmap = xchk_xattr_usedmap(sc);
	struct xfs_attr_leaf_name_local	*lentry;
	struct xfs_attr_leaf_name_remote *rentry;
	struct xfs_attr_leaf_entry	*ent;
	struct xfs_attr_leaf_entry	*entries;
	char				*buf_end;
	size_t				off;
	unsigned int			nameidx;
	unsigned int			hdrsize;
	int				i;
	int				error = 0;

	bitmap_zero(usedmap, mp->m_attr_geo->blksize);

	/* Check the leaf header */
	leaf = bp->b_addr;
	xfs_attr3_leaf_hdr_from_disk(mp->m_attr_geo, &leafhdr, leaf);
	hdrsize = xfs_attr3_leaf_hdr_size(leaf);
	xchk_xattr_set_map(sc, usedmap, 0, hdrsize);
	entries = xfs_attr3_leaf_entryp(leaf);

	buf_end = (char *)bp->b_addr + mp->m_attr_geo->blksize;
	for (i = 0, ent = entries; i < leafhdr.count; ent++, i++) {
		if (xchk_should_terminate(sc, &error))
			break;

		/* Skip key if it conflicts with something else? */
		off = (char *)ent - (char *)leaf;
		if (!xchk_xattr_set_map(sc, usedmap, off,
				sizeof(xfs_attr_leaf_entry_t)))
			continue;

		/* Check the name information. */
		nameidx = be16_to_cpu(ent->nameidx);
		if (nameidx < leafhdr.firstused ||
		    nameidx >= mp->m_attr_geo->blksize)
			continue;

		if (ent->flags & XFS_ATTR_LOCAL) {
			lentry = xfs_attr3_leaf_name_local(leaf, i);
			error = xrep_xattr_salvage_local_attr(rx, ent, nameidx,
					buf_end, lentry);
		} else {
			rentry = xfs_attr3_leaf_name_remote(leaf, i);
			error = xrep_xattr_salvage_remote_attr(rx, ent, nameidx,
					buf_end, rentry, i, bp);
		}
		if (error)
			break;
	}

	return error;
}

/* Try to recover shortform attrs. */
STATIC int
xrep_xattr_recover_sf(
	struct xrep_xattr		*rx)
{
	struct xfs_attr_shortform	*sf;
	struct xfs_attr_sf_entry	*sfe;
	struct xfs_attr_sf_entry	*next;
	struct xfs_ifork		*ifp;
	unsigned char			*end;
	int				i;
	int				error;

	ifp = XFS_IFORK_PTR(rx->sc->ip, XFS_ATTR_FORK);
	sf = (struct xfs_attr_shortform *)rx->sc->ip->i_afp->if_u1.if_data;
	end = (unsigned char *)ifp->if_u1.if_data + ifp->if_bytes;

	for (i = 0, sfe = &sf->list[0]; i < sf->hdr.count; i++) {
		if (xchk_should_terminate(rx->sc, &error))
			break;

		next = XFS_ATTR_SF_NEXTENTRY(sfe);
		if ((unsigned char *)next > end)
			break;

		/* Ok, let's save this key/value. */
		error = xrep_xattr_salvage_sf_attr(rx, sfe);
		if (error)
			return error;

		sfe = next;
	}

	return 0;
}

/*
 * Blindly walk every block in an attr fork without stumbling over incore
 * buffers for remote attr value blocks.
 *
 * Attribute leaf and node blocks are simple -- they're a single block, so we
 * can walk them one at a time and we never have to worry about discontiguous
 * multiblock buffers like we do for directories.
 *
 * Unfortunately, remote attr blocks add a lot of complexity here.  Each disk
 * block is totally self contained, in the sense that the v5 header provides no
 * indication that there could be more data in the next block.  The incore
 * buffers can span multiple blocks, though they never cross extent records.
 * However, they don't necessarily start or end on an extent record boundary.
 *
 * Because the buffer cache get function complains if it finds a buffer
 * matching the block number but not matching the length, we must be careful to
 * look for incore buffers (up to the maximum length of a remote value) that
 * could be hiding anywhere in the extent record.  If we find an incore buffer,
 * we can pass that to the callback function.  Otherwise, read a single block
 * and pass that to the callback.  Note the subtlety that remote attr value
 * blocks for which there is no incore buffer will be passed to the callback
 * one block at a time.
 *
 * The caller must hold the ILOCK.  We use XBF_TRYLOCK here to skip any locked
 * buffer on the assumption that we don't own the block and don't want to hang
 * the system on a potentially garbage buffer.
 *
 * XREP_ATTR_WALK_INCORE: don't read buffers from disk.
 */
#define XREP_ATTR_WALK_INCORE	(1U << 0)
STATIC int
xrep_attr_walk_blind(
	struct xfs_inode	*ip,
	unsigned int		flags,
	int			(*fn)(struct xfs_inode *ip, xfs_dablk_t dabno,
				      struct xfs_buf *bp, void *priv),
	void			*priv)
{
	struct xfs_bmbt_irec	map;
	struct xfs_mount	*mp = ip->i_mount;
	xfs_fileoff_t		offset = 0;
	xfs_fileoff_t		end = XFS_MAX_FILEOFF;
	xfs_filblks_t		len;
	xfs_fsblock_t		fsbno;
	xfs_dablk_t		dabno;
	int			max_rmt_blocks;
	int			nmap;
	int			error = 0;

	ASSERT(ip->i_mount->m_attr_geo->fsbcount == 1);

	max_rmt_blocks = xfs_attr3_rmt_blocks(mp, XFS_XATTR_SIZE_MAX);

	for (offset = 0;
	     offset < end;
	     offset = map.br_startoff + map.br_blockcount) {
		/* Walk the attr fork piece by piece... */
		nmap = 1;
		error = xfs_bmapi_read(ip, offset, end - offset,
				&map, &nmap, XFS_BMAPI_ATTRFORK);
		if (error)
			return error;
		if (nmap != 1)
			return -EFSCORRUPTED;
		if (!xfs_bmap_is_real_extent(&map))
			continue;

		for (dabno = map.br_startoff, fsbno = map.br_startblock;
		     dabno < map.br_startoff + map.br_blockcount;
		     dabno += len, fsbno += len) {
			struct xfs_buf	*bp;
			xfs_daddr_t	daddr = XFS_FSB_TO_DADDR(mp, fsbno);

			len = min_t(xfs_filblks_t, map.br_blockcount,
					max_rmt_blocks);

			/*
			 * Look for an incore buffer for every possible rmt
			 * or leaf block that could start at this physical
			 * position.
			 */
			while (len > 0) {
				bp = xfs_buf_incore(mp->m_ddev_targp, daddr,
						XFS_FSB_TO_BB(mp, len),
						XBF_TRYLOCK | XBF_SCAN_STALE);
				if (bp)
					goto dispatch_fn;

				len--;
			}

			/*
			 * If we didn't find an incore buffer, reset len to 1
			 * so that we can make forward progress.
			 */
			len = 1;

			if (flags & XREP_ATTR_WALK_INCORE)
				continue;

			error = xfs_buf_read(mp->m_ddev_targp, daddr,
					XFS_FSB_TO_BB(mp, len),
					XBF_TRYLOCK, &bp, NULL);
			if (error)
				return error;

dispatch_fn:
			/* Call the callback function. */
			error = fn(ip, dabno, bp, priv);
			xfs_buf_relse(bp);
			if (error)
				return error;
		}
	}

	return 0;
}

/* Deal with a buffer that we found during our walk of the attr fork. */
STATIC int
xrep_xattr_recover_block(
	struct xfs_inode	*ip,
	xfs_dablk_t		dabno,
	struct xfs_buf		*bp,
	void			*priv)
{
	struct xrep_xattr	*rx = priv;
	struct xfs_da_blkinfo	*info = bp->b_addr;
	int			error = 0;

	trace_xrep_xattr_recover_leafblock(rx->sc->ip, dabno,
			be16_to_cpu(info->magic));

	/*
	 * If the buffer has the right magic number for an attr leaf block and
	 * passes a structure check (we don't care about checksums), salvage
	 * as much as we can from the block. */
	if (info->magic == cpu_to_be16(XFS_ATTR3_LEAF_MAGIC) &&
	    xrep_buf_verify_struct(bp, &xfs_attr3_leaf_buf_ops))
		error = xrep_xattr_recover_leaf(rx, bp);

	/*
	 * If the buffer didn't already have buffer ops set, mark it stale so
	 * that it doesn't hang around in memory to cause problems.
	 */
	if (bp->b_ops == NULL)
		xfs_buf_stale(bp);
	return error;
}

/* Extract as many attribute keys and values as we can. */
STATIC int
xrep_xattr_recover(
	struct xrep_xattr	*rx)
{
	struct xfs_scrub	*sc = rx->sc;
	int			error = 0;

	if (sc->ip->i_d.di_aformat == XFS_DINODE_FMT_LOCAL)
		return xrep_xattr_recover_sf(rx);

	/*
	 * Set the xchk_attr_buf to be as large as we're going to need it to be
	 * to compute space usage bitmaps for each attr block we try to
	 * salvage.  We don't salvage attrs whose name and value areas are
	 * crosslinked with anything else.
	 */
	error = xchk_setup_xattr_buf(sc, 0, KM_MAYFAIL);
	if (error == -ENOMEM)
		return -EDEADLOCK;
	if (error)
		return error;

	return xrep_attr_walk_blind(sc->ip, 0, xrep_xattr_recover_block, rx);
}

/*
 * Reset the extended attribute fork to a state where we can start re-adding
 * the salvaged attributes.
 */
STATIC void
xrep_xattr_fork_remove(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	struct xfs_attr_sf_hdr	*hdr;
	struct xfs_ifork	*ifp;

	/*
	 * If the data fork is in btree format, we can't change di_forkoff
	 * because we could run afoul of the rule that the data fork isn't
	 * supposed to be in btree format if there's enough space in the fork
	 * that it could have used extents format.  Instead, reinitialize the
	 * attr fork to have a shortform structure with zero attributes.
	 */
	if (ip->i_d.di_format == XFS_DINODE_FMT_BTREE) {
		ip->i_d.di_aformat = XFS_DINODE_FMT_LOCAL;
		ifp = XFS_IFORK_PTR(ip, XFS_ATTR_FORK);
		ifp->if_flags &= ~XFS_IFEXTENTS;
		ifp->if_flags |= XFS_IFINLINE;
		xfs_idata_realloc(ip, (int)sizeof(*hdr) - ifp->if_bytes,
				XFS_ATTR_FORK);
		hdr = (struct xfs_attr_sf_hdr *)ifp->if_u1.if_data;
		hdr->count = 0;
		hdr->totsize = cpu_to_be16(sizeof(*hdr));
		xfs_trans_log_inode(sc->tp, ip,
				XFS_ILOG_CORE | XFS_ILOG_ADATA);
		return;
	}

	xfs_attr_fork_remove(ip, sc->tp);
}

/* Rip the buffer ops off a block so that it can be marked stale. */
STATIC int
xrep_xattr_stale_block(
	struct xfs_inode	*ip,
	xfs_dablk_t		dabno,
	struct xfs_buf		*bp,
	void			*priv)
{
	xfs_buf_stale(bp);
	return 0;
}

/*
 * Free all the attribute fork blocks and delete the fork.  The caller must
 * join the inode to the transaction.  This function returns with the inode
 * joined to a clean scrub transaction.
 */
int
xrep_xattr_reset_fork(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	int			error;

	ASSERT(ip == sc->ip || ip == sc->tempip);

	if (ip->i_d.di_aformat == XFS_DINODE_FMT_LOCAL)
		goto zap;

	/* Invalidate each attr block in the attr fork. */
	error = xrep_attr_walk_blind(ip, XREP_ATTR_WALK_INCORE,
			xrep_xattr_stale_block, NULL);
	if (error)
		return error;

	/* Now free all the blocks. */
	error = xfs_bunmapi_range(&sc->tp, ip, XFS_ATTR_FORK, 0,
			XFS_MAX_FILEOFF, XFS_BMAPI_NODISCARD);
	if (error)
		return error;

zap:
	xrep_xattr_fork_remove(sc, ip);
	return xrep_roll_trans(sc);
}

/*
 * Compare two xattr keys.  ATTR_SECURE keys come before ATTR_ROOT and
 * ATTR_ROOT keys come before user attrs.  Otherwise sort in hash order.
 */
static int
xrep_xattr_key_cmp(
	const void			*a,
	const void			*b)
{
	const struct xrep_xattr_key	*ap = a;
	const struct xrep_xattr_key	*bp = b;

	if (ap->flags > bp->flags)
		return 1;
	else if (ap->flags < bp->flags)
		return -1;

	if (ap->hash > bp->hash)
		return 1;
	else if (ap->hash < bp->hash)
		return -1;
	return 0;
}

/*
 * Find all the extended attributes for this inode by scraping them out of the
 * attribute key blocks by hand.  The caller must clean up the lists if
 * anything goes wrong.
 */
STATIC int
xrep_xattr_find_attributes(
	struct xrep_xattr	*rx)
{
	struct xfs_inode	*ip = rx->sc->ip;
	struct xfs_ifork	*ifp;
	int			error;

	error = xrep_ino_dqattach(rx->sc);
	if (error)
		return error;

	/* Extent map should be loaded. */
	ifp = XFS_IFORK_PTR(ip, XFS_ATTR_FORK);
	if (XFS_IFORK_FORMAT(ip, XFS_ATTR_FORK) != XFS_DINODE_FMT_LOCAL &&
	    !(ifp->if_flags & XFS_IFEXTENTS)) {
		error = xfs_iread_extents(rx->sc->tp, ip, XFS_ATTR_FORK);
		if (error)
			return error;
	}

	/* Read every attr key and value and record them in memory. */
	error = xrep_xattr_recover(rx);
	if (error)
		return error;

	/*
	 * Reset the xchk_attr_buf to be as large as we're going to need it to
	 * be to store each attribute name and value as we re-add them to the
	 * file.  We must preallocate the memory here because once we start
	 * to modify the filesystem we cannot afford an ENOMEM.
	 */
	error = xchk_setup_xattr_buf(rx->sc, rx->max_valuelen, KM_MAYFAIL);
	if (error == -ENOMEM)
		return -EDEADLOCK;
	if (error)
		return error;

	return 0;
}

/* Insert one xattr key/value. */
STATIC int
xrep_xattr_insert_rec(
	const void			*item,
	void				*priv)
{
	struct xfs_da_args		args = { NULL };
	const struct xrep_xattr_key	*key = item;
	struct xrep_xattr		*rx = priv;
	unsigned char			*name = xchk_xattr_namebuf(rx->sc);
	unsigned char			*value = xchk_xattr_valuebuf(rx->sc);
	int				error;

	/*
	 * The attribute name is stored near the end of the in-core buffer,
	 * though we reserve one more byte to ensure null termination.
	 */
	name[XATTR_NAME_MAX] = 0;

	error = xblob_get(rx->xattr_blobs, key->name_cookie, name,
			key->namelen);
	if (error)
		return error;

	error = xblob_free(rx->xattr_blobs, key->name_cookie);
	if (error)
		return error;

	error = xblob_get(rx->xattr_blobs, key->value_cookie, value,
			key->valuelen);
	if (error)
		return error;

	error = xblob_free(rx->xattr_blobs, key->value_cookie);
	if (error)
		return error;

	name[key->namelen] = 0;

	trace_xrep_xattr_insert_rec(rx->sc->tempip, key->flags, name,
			key->namelen, key->valuelen);

	args.dp = rx->sc->tempip;
	args.attr_filter = key->flags;
	args.name = name;
	args.namelen = key->namelen;
	args.value = value;
	args.valuelen = key->valuelen;
	return xfs_attr_set(&args);
}

/*
 * Prepare both inodes' attribute forks for extent swapping.  Promote the
 * tempfile from short format to leaf format, and if the file being repaired
 * has a short format attr fork, turn it into an empty extent list.
 */
STATIC int
xrep_xattr_swap_prep(
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
		struct xfs_buf		*leaf_bp = NULL;
		struct xfs_da_args	args = {
			.dp		= sc->tempip,
			.geo		= sc->mp->m_attr_geo,
			.whichfork	= XFS_ATTR_FORK,
			.trans		= sc->tp,
			.total		= 1,
		};

		error = xfs_attr_shortform_to_leaf(&args, &leaf_bp);
		if (error)
			return error;

		/*
		 * Roll the deferred log items to get us back to a clean
		 * transaction.  Hold on to the leaf buffer across this roll
		 * so that the AIL cannot grab our half-baked block.
		 */
		xfs_trans_bhold(sc->tp, leaf_bp);
		error = xfs_defer_finish(&sc->tp);
		xfs_trans_bhold_release(sc->tp, leaf_bp);
	}

	/*
	 * If the file being repaired had a shortform attribute fork, convert
	 * that to an empty extent list in preparation for the atomic extent
	 * swap.
	 */
	if (ip_local) {
		struct xfs_ifork	*ifp;

		sc->ip->i_d.di_aformat = XFS_DINODE_FMT_EXTENTS;
		sc->ip->i_d.di_anextents = 0;

		ifp = XFS_IFORK_PTR(sc->ip, XFS_ATTR_FORK);
		xfs_ifork_reset(ifp);
		ifp->if_bytes = 0;
		ifp->if_u1.if_root = NULL;
		ifp->if_height = 0;
		ifp->if_flags |= XFS_IFEXTENTS;

		xfs_trans_log_inode(sc->tp, sc->ip,
				XFS_ILOG_CORE | XFS_ILOG_ADATA);
	}

	return 0;
}

/* State we need to track while rewriting attr block owners. */
struct xrep_xattr_swap_owner {
	struct xfs_attr_list_context	ctx;
	struct xbitmap			rmt_blocks;
	struct xfs_scrub		*sc;
};

/*
 * Change the owner field of a remote attribute value block to match the file
 * that's being repaired.  In-core buffers for these values span a single
 * extent and are never logged, so we must be careful to mask off the
 * corresponding range so that the leaf/node pass will skip these parts of the
 * attr fork mappings.
 */
static void
xrep_xattr_swap_rmt_owner(
	struct xfs_attr_list_context	*context,
	int				flags,
	unsigned char			*name,
	int				namelen,
	int				valuelen)
{
	struct xfs_da_args		args = {
		.op_flags		= XFS_DA_OP_NOTIME,
		.attr_filter		= flags & XFS_ATTR_NSP_ONDISK_MASK,
		.geo			= context->dp->i_mount->m_attr_geo,
		.whichfork		= XFS_ATTR_FORK,
		.dp			= context->dp,
		.name			= name,
		.namelen		= namelen,
		.hashval		= xfs_da_hashname(name, namelen),
		.trans			= context->tp,
		.value			= NULL,
		.valuelen		= 0,
	};
	LIST_HEAD(buffer_list);
	struct xfs_bmbt_irec		map;
	struct xrep_xattr_swap_owner	*xso;
	struct xfs_mount		*mp = context->dp->i_mount;
	struct xfs_attr3_rmt_hdr	*rmt;
	struct xfs_buf			*bp;
	void				*p;
	xfs_daddr_t			dblkno;
	int				dblkcnt;
	int				nmap;
	int				error;

	xso = container_of(context, struct xrep_xattr_swap_owner, ctx);

	if (flags & (XFS_ATTR_LOCAL | XFS_ATTR_INCOMPLETE))
		return;

	error = xfs_attr_get_ilocked(&args);
	if (error)
		goto fail;

	/*
	 * Mark this region of the attr fork so that the leaf/node scan will
	 * skip this part.
	 */
	error = xbitmap_set(&xso->rmt_blocks, args.rmtblkno, args.rmtblkcnt);
	if (error)
		goto fail;

	while (args.rmtblkcnt > 0) {
		nmap = 1;
		error = xfs_bmapi_read(args.dp, args.rmtblkno, args.rmtblkcnt,
				&map, &nmap, XFS_BMAPI_ATTRFORK);
		if (error || nmap != 1)
			goto fail;

		if (!xfs_bmap_is_real_extent(&map))
			goto fail;

		dblkno = XFS_FSB_TO_DADDR(mp, map.br_startblock);
		dblkcnt = XFS_FSB_TO_BB(mp, map.br_blockcount);
		error = xfs_buf_read(mp->m_ddev_targp, dblkno, dblkcnt, 0, &bp,
				&xfs_attr3_rmt_buf_ops);
		if (error)
			goto fail;

		/*
		 * Each rmt block within the buffer gets its own header, so
		 * update the owner for each header.
		 */
		for (p = bp->b_addr;
		     p < bp->b_addr + BBTOB(bp->b_length);
		     p += mp->m_attr_geo->blksize) {
			rmt = p;
			rmt->rm_owner = cpu_to_be64(xso->sc->ip->i_ino);
		}

		xfs_buf_delwri_queue(bp, &buffer_list);
		xfs_buf_relse(bp);

		/* roll attribute extent map forwards */
		args.rmtblkno += map.br_blockcount;
		args.rmtblkcnt -= map.br_blockcount;
	}

	/* Write the entire remote value to disk. */
	error = xfs_buf_delwri_submit(&buffer_list);
	if (error)
		goto fail;

	return;
fail:
	xfs_buf_delwri_cancel(&buffer_list);
	context->seen_enough = 1;
}

/*
 * Change the owner field of every block in the attribute fork to match the
 * file being repaired.  First we fix the remote value blocks (which have
 * particular incore geometries) and then change the rest one block at a time.
 */
STATIC int
xrep_xattr_swap_leaf_owner(
	struct xrep_xattr_swap_owner	*xso)
{
	struct xfs_bmbt_irec		map;
	struct xfs_da_geometry		*geo = xso->sc->mp->m_attr_geo;
	struct xfs_scrub		*sc = xso->sc;
	struct xfs_da3_blkinfo		*info;
	struct xfs_buf			*bp;
	xfs_fileoff_t			offset = 0;
	xfs_fileoff_t			end = -1U;
	xfs_dablk_t			dabno;
	int				nmap;
	int				error;

	for (offset = 0;
	     offset < end;
	     offset = map.br_startoff + map.br_blockcount) {
		nmap = 1;
		error = xfs_bmapi_read(sc->tempip, offset, end - offset,
				&map, &nmap, XFS_BMAPI_ATTRFORK);
		if (error)
			return error;
		if (nmap != 1)
			return -EFSCORRUPTED;
		if (!xfs_bmap_is_real_extent(&map)) {
			continue;
		}

		if (xbitmap_test(&xso->rmt_blocks, map.br_startoff,
				 &map.br_blockcount)) {
			continue;
		}

		for (dabno = round_up(map.br_startoff, geo->fsbcount);
		     dabno < map.br_startoff + map.br_blockcount;
		     dabno += geo->fsbcount) {
			error = xfs_da_read_buf(sc->tp, sc->tempip,
					dabno, 0, &bp, XFS_ATTR_FORK, NULL);
			if (error)
				return error;
			if (!bp)
				return -EFSCORRUPTED;

			info = bp->b_addr;
			info->owner = cpu_to_be64(sc->ip->i_ino);

			/* If nobody set a buffer type or ops, set them now. */
			if (bp->b_ops == NULL) {
				switch (info->hdr.magic) {
				case cpu_to_be16(XFS_ATTR3_LEAF_MAGIC):
					bp->b_ops = &xfs_attr3_leaf_buf_ops;
					break;
				case cpu_to_be16(XFS_DA3_NODE_MAGIC):
					bp->b_ops = &xfs_da3_node_buf_ops;
					break;
				default:
					xfs_trans_brelse(sc->tp, bp);
					return -EFSCORRUPTED;
				}
				xfs_buf_set_ref(bp, XFS_ATTR_BTREE_REF);
			}

			xfs_trans_ordered_buf(sc->tp, bp);
			xfs_trans_brelse(sc->tp, bp);
		}
	}

	return 0;
}
/*
 * Walk the temporary file's xattr blocks, setting the owner field of each
 * block to the new owner.  We use ordered and delwri buffers to flush
 * everything out to disk ahead of comitting the atomic extent swap.  Rewriting
 * the attr blocks like this is apparently safe because attr inactivation isn't
 * picky about owner field enforcement(!)
 */
STATIC int
xrep_xattr_swap_owner(
	struct xfs_scrub		*sc)
{
	struct xrep_xattr_swap_owner	xso = {
		.ctx.dp			= sc->tempip,
		.ctx.resynch		= 1,
		.ctx.put_listent	= xrep_xattr_swap_rmt_owner,
		.ctx.allow_incomplete	= false,
		.ctx.seen_enough	= 0,
		.ctx.tp			= sc->tp,
		.sc			= sc,
	};
	int				error;

	xbitmap_init(&xso.rmt_blocks);

	/* First pass -- change the owners of the remote blocks. */
	error = xfs_attr_list_ilocked(&xso.ctx);
	if (error)
		goto out;
	if (xso.ctx.seen_enough) {
		error = -EFSCORRUPTED;
		goto out;
	}

	/* Second pass -- change each attr leaf/node buffer. */
	error = xrep_xattr_swap_leaf_owner(&xso);
out:
	xbitmap_destroy(&xso.rmt_blocks);
	return error;
}

/*
 * If both files' attribute structure are in short format, we can copy
 * the short format data from the tempfile to the repaired file if it'll
 * fit.
 */
STATIC void
xrep_xattr_swap_local(
	struct xfs_scrub	*sc,
	int			newsize,
	int			forkoff)
{
	struct xfs_ifork	*ifp1, *ifp2;

	ifp1 = XFS_IFORK_PTR(sc->tempip, XFS_ATTR_FORK);
	ifp2 = XFS_IFORK_PTR(sc->ip, XFS_ATTR_FORK);
	sc->ip->i_d.di_forkoff = forkoff;

	xfs_idata_realloc(sc->ip, ifp1->if_bytes - ifp2->if_bytes,
			XFS_ATTR_FORK);

	memcpy(ifp2->if_u1.if_data, ifp1->if_u1.if_data, newsize);
	xfs_trans_log_inode(sc->tp, sc->ip, XFS_ILOG_CORE | XFS_ILOG_ADATA);
}

/* Swap the temporary file's attribute fork with the one being repaired. */
STATIC int
xrep_xattr_swap(
	struct xrep_xattr	*rx)
{
	struct xfs_swapext_req	req;
	struct xfs_swapext_res	res;
	struct xfs_scrub	*sc = rx->sc;
	bool			ip_local, temp_local;
	int			error;

	error = xrep_swapext_prep(rx->sc, XFS_ATTR_FORK, &req, &res);
	if (error)
		return error;

	error = xchk_trans_alloc(sc, res.resblks);
	if (error)
		return error;

	/*
	 * Lock and join the inodes to the tansaction so that transaction commit
	 * or cancel will unlock the inodes from this point onwards.
	 */
	xfs_lock_two_inodes(sc->ip, XFS_ILOCK_EXCL,
			    sc->tempip, XFS_ILOCK_EXCL);
	sc->temp_ilock_flags |= XFS_ILOCK_EXCL;
	sc->ilock_flags |= XFS_ILOCK_EXCL;
	xfs_trans_ijoin(sc->tp, sc->ip, 0);
	xfs_trans_ijoin(sc->tp, sc->tempip, 0);

	ip_local = XFS_IFORK_FORMAT(sc->ip, XFS_ATTR_FORK) ==
				XFS_DINODE_FMT_LOCAL;
	temp_local = XFS_IFORK_FORMAT(sc->tempip, XFS_ATTR_FORK) ==
				XFS_DINODE_FMT_LOCAL;

	/*
	 * If the both files have a local format attr fork and the rebuilt
	 * xattr data would fit in the repaired file's attr fork, just copy
	 * the contents from the tempfile and declare ourselves done.
	 */
	if (ip_local && temp_local) {
		int	forkoff;
		int	newsize;

		newsize = XFS_ATTR_SF_TOTSIZE(sc->tempip);
		forkoff = xfs_attr_shortform_bytesfit(sc->ip, newsize);
		if (forkoff > 0) {
			xrep_xattr_swap_local(sc, newsize, forkoff);
			return 0;
		}
	}

	/* Otherwise, make sure both attr forks are in block-mapping mode. */
	error = xrep_xattr_swap_prep(sc, temp_local, ip_local);
	if (error)
		return error;

	/* Rewrite the owner field of all attr blocks in the temporary file. */
	error = xrep_xattr_swap_owner(sc);
	if (error)
		return error;

	return xfs_swapext_atomic(&sc->tp, &req);
}

/*
 * Insert into the tempfile all the attributes that we collected.
 *
 * Commit the repair transaction and drop the ilock because the attribute
 * setting code needs to be able to allocate special transactions and take the
 * ilock on its own.  The attributes are added to the temporary file (which can
 * be disposed of easily on failure).  If we finish rebuilding all of the
 * salvageable attrs, we can then use atomic extent swapping to commit the
 * new attr index to the file.
 */
STATIC int
xrep_xattr_rebuild_tree(
	struct xrep_xattr	*rx)
{
	uint64_t		nr_attrs = xfbma_length(rx->xattr_records);
	int			error;

	/* Nothing to salvage?  Zap the attr fork and finish. */
	if (nr_attrs == 0) {
		xfs_trans_ijoin(rx->sc->tp, rx->sc->ip, 0);
		return xrep_xattr_reset_fork(rx->sc, rx->sc->ip);
	}

	/*
	 * Commit the repair transaction and drop the ILOCK so that we can
	 * use individual transactions to re-add each extended attribute.
	 */
	error = xfs_trans_commit(rx->sc->tp);
	rx->sc->tp = NULL;
	if (error)
		return error;

	/*
	 * Drop the ILOCK so that we don't pin the tail of the log.  We still
	 * hold the IOLOCK (aka i_rwsem) which will prevent attr modifications,
	 * but there's nothing to prevent userspace from reading/listing the
	 * attrs while we build a new attr fork.  Oh well, at least the fs
	 * can't shut down those threads if they stumble into corrupt blocks.
	 */
	xfs_iunlock(rx->sc->ip, XFS_ILOCK_EXCL);
	rx->sc->ilock_flags &= ~XFS_ILOCK_EXCL;

	/*
	 * Sort the attribute keys by hash to minimize dabtree splits when we
	 * rebuild the extended attribute information.
	 */
	error = xfbma_sort(rx->xattr_records, xrep_xattr_key_cmp);
	if (error)
		return error;

	/* Add every attr to the tempfile. */
	error = xfbma_iter_del(rx->xattr_records, xrep_xattr_insert_rec, rx);
	if (error)
		return error;

	/*
	 * Swap the tempfile's attr fork with the file being repaired.  This
	 * recreates the transaction and re-takes the ILOCK in the scrub
	 * context.
	 */
	error = xrep_xattr_swap(rx);
	if (error)
		return error;

	/*
	 * Now wipe out the attr fork of the temp file so that regular inode
	 * inactivation won't trip over the corrupt attr fork.
	 */
	return xrep_xattr_reset_fork(rx->sc, rx->sc->tempip);
}

/*
 * Repair the extended attribute metadata.
 *
 * XXX: Remote attribute value buffers encompass the entire (up to 64k) buffer.
 * The buffer cache in XFS can't handle aliased multiblock buffers, so this
 * might misbehave if the attr fork is crosslinked with other filesystem
 * metadata.
 */
int
xrep_xattr(
	struct xfs_scrub	*sc)
{
	struct xrep_xattr	rx = {
		.sc		= sc,
	};
	int			error;

	if (!xfs_inode_hasattr(sc->ip))
		return -ENOENT;

	/* Set up some storage */
	rx.xattr_records = xfbma_init(sizeof(struct xrep_xattr_key));
	if (IS_ERR(rx.xattr_records))
		return PTR_ERR(rx.xattr_records);
	rx.xattr_blobs = xblob_init();
	if (IS_ERR(rx.xattr_blobs)) {
		error = PTR_ERR(rx.xattr_blobs);
		goto out_arr;
	}

	/* Collect extended attributes by parsing raw blocks. */
	error = xrep_xattr_find_attributes(&rx);
	if (error)
		goto out;

	/* Now rebuild the attribute information. */
	error = xrep_xattr_rebuild_tree(&rx);
out:
	xblob_destroy(rx.xattr_blobs);
out_arr:
	xfbma_destroy(rx.xattr_records);
	return error;
}
