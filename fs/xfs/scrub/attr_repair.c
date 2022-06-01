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
#include "xfs_xchgrange.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/tempfile.h"
#include "scrub/tempswap.h"
#include "scrub/xfarray.h"
#include "scrub/xfblob.h"
#include "scrub/attr.h"

/*
 * Extended Attribute Repair
 * =========================
 *
 * We repair extended attributes by reading the xattr leaf blocks looking for
 * attributes.  Salvaged attrs are added to a private hidden temporary file.
 * When we're done salvaging, we rewrite the xattr block owners and use an
 * atomic extent swap to commit the new xattr blocks to the file being
 * repaired.
 */

struct xrep_xattr_key {
	/* Cookie for retrieval of the xattr name. */
	xfblob_cookie		name_cookie;

	/* Cookie for retrieval of the xattr value. */
	xfblob_cookie		value_cookie;

	/* Hash of the dirent name. */
	unsigned int		hash;

	/* XFS_ATTR_* flags */
	int			flags;

	/* Length of the value and name. */
	uint32_t		valuelen;
	uint16_t		namelen;
};

struct xrep_xattr {
	struct xfs_scrub	*sc;

	struct xrep_tempswap	tx;

	/* xattr keys */
	struct xfarray		*xattr_records;

	/* xattr values */
	struct xfblob		*xattr_blobs;

	/* Number of attributes that we are salvaging. */
	unsigned long long	attrs_found;
};

/* Absorb up to 8 pages of attrs before we flush them to the temp file. */
#define XREP_XATTR_SALVAGE_BYTES	(PAGE_SIZE * 8)

/* Set up to recreate the extended attributes. */
int
xrep_setup_xattr(
	struct xfs_scrub	*sc)
{
	return xrep_tempfile_create(sc, S_IFREG);
}

/*
 * Allocate enough memory to hold whatever we need to salvage an attr block.
 * Buffer contents can be preserved, unlike in the scrub counterpart to this
 * function.
 */
STATIC int
xrep_setup_xattr_buf(
	struct xfs_scrub	*sc,
	size_t			value_size,
	bool			preserve)
{
	size_t			sz;
	struct xchk_xattr_buf	*new_ab;
	struct xchk_xattr_buf	*ab = sc->buf;

	ASSERT(!preserve || ab != NULL);

	/*
	 * We need enough space to hold a bitmap for the used space within an
	 * attr block; the name of a salvaged attr; and the value of a salvaged
	 * attr.
	 */
	sz = sizeof(long) * BITS_TO_LONGS(sc->mp->m_attr_geo->blksize) +
			value_size + XATTR_NAME_MAX + 1;

	/*
	 * If there's already a buffer, figure out if we need to reallocate it
	 * to accommodate a larger size.
	 */
	if (ab && ab->sz >= sz)
		return 0;

	/* Give back the old memory as soon as we can, to reduce pressure. */
	if (!preserve && ab) {
		kvfree(ab);
		ab = NULL;
	}

	new_ab = kvmalloc(sizeof(struct xchk_xattr_buf) + sz, XCHK_GFP_FLAGS);
	if (!new_ab)
		return -ENOMEM;

	if (ab) {
		memcpy(new_ab, ab, ab->sz);
		kvfree(ab);
	}
	new_ab->sz = sz;
	sc->buf = new_ab;
	return 0;
}

/*
 * While we're salvaging the contents of an xattr block, the first part of the
 * buffer contains a bitmap of the parts of the block that we've already seen.
 * Therefore, salvaged values /must/ be stored after the bitmap.
 */
static inline unsigned char *
xrep_xattr_salvage_valuebuf(
	struct xfs_scrub	*sc)
{
	return (unsigned char *)(xchk_xattr_usedmap(sc) +
				 BITS_TO_LONGS(sc->mp->m_attr_geo->blksize));
}

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
	};
	unsigned int		i = 0;
	int			error = 0;

	if (xchk_should_terminate(rx->sc, &error))
		return error;

	/*
	 * Truncate the name to the first character that would trip namecheck.
	 * If we no longer have a name after that, ignore this attribute.
	 */
	while (i < namelen && name[i] != 0)
		i++;
	if (i == 0)
		return 0;
	key.namelen = i;
	key.hash = xfs_da_hashname(name, key.namelen);

	trace_xrep_xattr_salvage_key(rx->sc->ip, key.flags, name, key.namelen,
			key.valuelen);

	error = xfblob_store(rx->xattr_blobs, &key.name_cookie, name,
			key.namelen);
	if (error)
		return error;

	error = xfblob_store(rx->xattr_blobs, &key.value_cookie, value,
			key.valuelen);
	if (error)
		return error;

	error = xfarray_append(rx->xattr_records, &key);
	if (error)
		return error;

	rx->attrs_found++;
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
	 * Enlarge the buffer (if needed) to hold the value that we're trying
	 * to salvage from the old extended attribute data.  The usedmap
	 * pointer itself may be invalid after this point, but we must keep the
	 * bitmap.
	 */
	error = xrep_setup_xattr_buf(rx->sc, valuelen, true);
	if (error == -ENOMEM)
		error = -EDEADLOCK;
	if (error)
		return error;
	value = xrep_xattr_salvage_valuebuf(rx->sc);

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
			return error;

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
			return error;
	}

	return 0;
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
	int				error = 0;

	ifp = XFS_IFORK_PTR(rx->sc->ip, XFS_ATTR_FORK);
	sf = (struct xfs_attr_shortform *)rx->sc->ip->i_afp->if_u1.if_data;
	end = (unsigned char *)ifp->if_u1.if_data + ifp->if_bytes;

	for (i = 0, sfe = &sf->list[0]; i < sf->hdr.count; i++) {
		if (xchk_should_terminate(rx->sc, &error))
			return error;

		next = xfs_attr_sf_nextentry(sfe);
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
 * Try to return a buffer of xattr data for a given physical extent.
 *
 * Because the buffer cache get function complains if it finds a buffer
 * matching the block number but not matching the length, we must be careful to
 * look for incore buffers (up to the maximum length of a remote value) that
 * could be hiding anywhere in the physical range.  If we find an incore
 * buffer, we can pass that to the caller.  Optionally, read a single block and
 * pass that back.
 *
 * Note the subtlety that remote attr value blocks for which there is no incore
 * buffer will be passed to the callback one block at a time.  These buffers
 * will not have any ops attached and must be staled to prevent aliasing with
 * multiblock buffers once we drop the ILOCK.
 */
STATIC int
xrep_xattr_find_buf(
	struct xfs_mount	*mp,
	xfs_fsblock_t		fsbno,
	xfs_extlen_t		max_len,
	bool			can_read,
	struct xfs_buf		**bpp)
{
	struct xrep_buf_scan	scan = {
		.daddr		= XFS_FSB_TO_DADDR(mp, fsbno),
		.max_sectors	= xrep_max_buf_sectors(mp, max_len),
		.daddr_step	= XFS_FSB_TO_BB(mp, 1),
	};
	struct xfs_buf		*bp;

	while ((bp = xrep_buf_scan_advance(mp, &scan)) != NULL) {
		*bpp = bp;
		return 0;
	}

	if (!can_read) {
		*bpp = NULL;
		return 0;
	}

	return xfs_buf_read(mp->m_ddev_targp, scan.daddr, XFS_FSB_TO_BB(mp, 1),
			XBF_TRYLOCK, bpp, NULL);
}

/*
 * Deal with a buffer that we found during our walk of the attr fork.
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
 * Therefore, we need a special buffer find function to walk the buffer cache
 * for us.
 *
 * The caller must hold the ILOCK on the file being repaired.  We use
 * XBF_TRYLOCK here to skip any locked buffer on the assumption that we don't
 * own the block and don't want to hang the system on a potentially garbage
 * buffer.
 */
STATIC int
xrep_xattr_recover_block(
	struct xrep_xattr	*rx,
	xfs_dablk_t		dabno,
	xfs_fsblock_t		fsbno,
	xfs_extlen_t		max_len,
	xfs_extlen_t		*actual_len)
{
	struct xfs_da_blkinfo	*info;
	struct xfs_buf		*bp;
	int			error;

	error = xrep_xattr_find_buf(rx->sc->mp, fsbno, max_len, true, &bp);
	if (error)
		return error;
	info = bp->b_addr;
	*actual_len = XFS_BB_TO_FSB(rx->sc->mp, bp->b_length);

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
	 * If the buffer didn't already have buffer ops set, it was read in by
	 * the _find_buf function and could very well be /part/ of a multiblock
	 * remote block.  Mark it stale so that it doesn't hang around in
	 * memory to cause problems.
	 */
	if (bp->b_ops == NULL)
		xfs_buf_stale(bp);

	xfs_buf_relse(bp);
	return error;
}

/* Insert one xattr key/value. */
STATIC int
xrep_xattr_insert_rec(
	struct xrep_xattr		*rx,
	const struct xrep_xattr_key	*key)
{
	struct xfs_da_args		args = {
		.dp			= rx->sc->tempip,
		.attr_filter		= key->flags,
		.attr_flags		= XATTR_CREATE,
		.namelen		= key->namelen,
		.valuelen		= key->valuelen,
		.op_flags		= XFS_DA_OP_NOTIME,
	};
	unsigned char			*name;
	int				error;

	/*
	 * Grab pointers to the scrub buffer so that we can use them to insert
	 * attrs into the temp file.  Because the salvage step should have made
	 * the buffer large enough for (a block bitmap + the largest value
	 * found + the largest possible attr name), it should be safe to use
	 * xfs_xattr_usedmap to copy values.
	 */
	args.name = name = xchk_xattr_namebuf(rx->sc);
	args.value = (unsigned char *)xchk_xattr_usedmap(rx->sc);

	/*
	 * The attribute name is stored near the end of the in-core buffer,
	 * though we reserve one more byte to ensure null termination.
	 */
	name[XATTR_NAME_MAX] = 0;

	error = xfblob_load(rx->xattr_blobs, key->name_cookie, name,
			key->namelen);
	if (error)
		return error;

	error = xfblob_free(rx->xattr_blobs, key->name_cookie);
	if (error)
		return error;

	error = xfblob_load(rx->xattr_blobs, key->value_cookie, args.value,
			key->valuelen);
	if (error)
		return error;

	error = xfblob_free(rx->xattr_blobs, key->value_cookie);
	if (error)
		return error;

	name[key->namelen] = 0;

	trace_xrep_xattr_insert_rec(rx->sc->tempip, key->flags, name,
			key->namelen, key->valuelen);

	/*
	 * xfs_attr_set creates and commits its own transaction.  If the attr
	 * already exists, we'll just drop it during the rebuild.
	 */
	error = xfs_attr_set(&args);
	if (error == -EEXIST)
		error = 0;

	return error;
}

/*
 * Periodically flush salvaged attributes to the temporary file.  This is done
 * to reduce the memory requirements of the xattr rebuild because files can
 * contain millions of attributes.
 */
STATIC int
xrep_xattr_flush_salvaged(
	struct xrep_xattr	*rx)
{
	xfarray_idx_t		array_cur;
	int			error;

	/*
	 * Entering this function, the scrub context has a reference to the
	 * inode being repaired, the temporary file, and a scrub transaction
	 * that we use during xattr salvaging to avoid livelocking if there
	 * are cycles in the xattr structures.  We hold ILOCK_EXCL on both
	 * the inode being repaired, though it is not ijoined to the scrub
	 * transaction.
	 *
	 * To constrain kernel memory use, we occasionally flush salvaged
	 * xattrs from the xfarray and xfblob structures into the temporary
	 * file in preparation for swapping the xattr structures at the end.
	 * Updating the temporary file requires a transaction, so we commit the
	 * scrub transaction and drop the two ILOCKs so that xfs_attr_set can
	 * allocate whatever transaction it wants.
	 *
	 * We still hold IOLOCK_EXCL on the inode being repaired, which
	 * prevents anyone from accessing the damaged xattr data while we
	 * repair it.
	 */
	error = xrep_trans_commit(rx->sc);
	if (error)
		return error;
	xchk_iunlock(rx->sc, XFS_ILOCK_EXCL);

	/*
	 * Take the IOLOCK of the temporary file while we modify xattrs.  This
	 * isn't strictly required because the temporary file is never revealed
	 * to userspace, but we follow the same locking rules.
	 */
	while (!xrep_tempfile_iolock_nowait(rx->sc)) {
		if (xchk_should_terminate(rx->sc, &error))
			return error;
		delay(1);
	}

	/* Add all the salvaged attrs to the temporary file. */
	foreach_xfarray_idx(rx->xattr_records, array_cur) {
		struct xrep_xattr_key	key;

		error = xfarray_load(rx->xattr_records, array_cur, &key);
		if (error)
			return error;

		error = xrep_xattr_insert_rec(rx, &key);
		if (error)
			return error;
	}
	xrep_tempfile_iounlock(rx->sc);

	/* Empty out both arrays now that we've added the entries. */
	xfarray_truncate(rx->xattr_records);
	xfblob_truncate(rx->xattr_blobs);

	/* Recreate the salvage transaction and relock the inode. */
	error = xchk_trans_alloc(rx->sc, 0);
	if (error)
		return error;
	xchk_ilock(rx->sc, XFS_ILOCK_EXCL);
	return 0;
}

/*
 * Decide if we need to flush the xattrs we've salvaged to disk to constrain
 * memory usage.
 */
static int
xrep_xattr_need_flush(
	struct xrep_xattr	*rx,
	bool			*need)
{
	long long		key_bytes, value_bytes;

	key_bytes = xfarray_bytes(rx->xattr_records);
	if (key_bytes < 0)
		return key_bytes;

	value_bytes = xfblob_bytes(rx->xattr_blobs);
	if (value_bytes < 0)
		return value_bytes;

	*need = key_bytes + value_bytes >= XREP_XATTR_SALVAGE_BYTES;
	return 0;
}

/* Extract as many attribute keys and values as we can. */
STATIC int
xrep_xattr_recover(
	struct xrep_xattr	*rx)
{
	struct xfs_bmbt_irec	got;
	struct xfs_scrub	*sc = rx->sc;
	struct xfs_da_geometry	*geo = sc->mp->m_attr_geo;
	xfs_fileoff_t		offset;
	xfs_extlen_t		len;
	xfs_dablk_t		dabno;
	int			nmap;
	int			error;

	/*
	 * Iterate each xattr leaf block in the attr fork to scan them for any
	 * attributes that we might salvage.
	 */
	for (offset = 0;
	     offset < XFS_MAX_FILEOFF;
	     offset = got.br_startoff + got.br_blockcount) {
		nmap = 1;
		error = xfs_bmapi_read(sc->ip, offset, XFS_MAX_FILEOFF - offset,
				&got, &nmap, XFS_BMAPI_ATTRFORK);
		if (error)
			return error;
		if (nmap != 1)
			return -EFSCORRUPTED;
		if (!xfs_bmap_is_written_extent(&got))
			continue;

		for (dabno = round_up(got.br_startoff, geo->fsbcount);
		     dabno < got.br_startoff + got.br_blockcount;
		     dabno += len) {
			xfs_fileoff_t	curr_offset = dabno - got.br_startoff;
			xfs_extlen_t	maxlen;
			bool		need_flush;

			if (xchk_should_terminate(rx->sc, &error))
				return error;

			maxlen = min_t(xfs_filblks_t, INT_MAX,
					got.br_blockcount - curr_offset);
			error = xrep_xattr_recover_block(rx, dabno,
					curr_offset + got.br_startblock,
					maxlen, &len);
			if (error)
				return error;

			error = xrep_xattr_need_flush(rx, &need_flush);
			if (error)
				return error;

			if (need_flush) {
				error = xrep_xattr_flush_salvaged(rx);
				if (error)
					return error;
			}
		}
	}

	return 0;
}

/*
 * Reset the extended attribute fork to a state where we can start re-adding
 * the salvaged attributes.
 */
STATIC int
xrep_xattr_fork_remove(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip)
{
	struct xfs_attr_sf_hdr	*hdr;
	struct xfs_ifork	*ifp = XFS_IFORK_PTR(ip, XFS_ATTR_FORK);

	/*
	 * If the data fork is in btree format, we can't change di_forkoff
	 * because we could run afoul of the rule that the data fork isn't
	 * supposed to be in btree format if there's enough space in the fork
	 * that it could have used extents format.  Instead, reinitialize the
	 * attr fork to have a shortform structure with zero attributes.
	 */
	if (ip->i_df.if_format == XFS_DINODE_FMT_BTREE) {
		ifp->if_format = XFS_DINODE_FMT_LOCAL;
		xfs_idata_realloc(ip, (int)sizeof(*hdr) - ifp->if_bytes,
				XFS_ATTR_FORK);
		hdr = (struct xfs_attr_sf_hdr *)ifp->if_u1.if_data;
		hdr->count = 0;
		hdr->totsize = cpu_to_be16(sizeof(*hdr));
		xfs_trans_log_inode(sc->tp, ip,
				XFS_ILOG_CORE | XFS_ILOG_ADATA);
		return 0;
	}

	/* If we still have attr fork extents, something's wrong. */
	if (ifp->if_nextents != 0) {
		struct xfs_iext_cursor	icur;
		struct xfs_bmbt_irec	irec;
		unsigned int		i = 0;

		xfs_emerg(sc->mp,
	"inode 0x%llx attr fork still has %llu attr extents, format %d?!",
				ip->i_ino, ifp->if_nextents, ifp->if_format);
		for_each_xfs_iext(ifp, &icur, &irec) {
			xfs_err(sc->mp,
	"[%u]: startoff %llu startblock %llu blockcount %llu state %u",
					i++, irec.br_startoff,
					irec.br_startblock, irec.br_blockcount,
					irec.br_state);
		}
		ASSERT(0);
		return -EFSCORRUPTED;
	}

	xfs_attr_fork_remove(ip, sc->tp);
	return 0;
}

/*
 * Free all the attribute fork blocks and delete the fork.  The caller must
 * ILOCK the file being repaired and ijoin it to the transaction.  This
 * function returns with the inode joined to a clean scrub transaction.
 */
int
xrep_xattr_reset_fork(
	struct xfs_scrub	*sc)
{
	int			error;

	/* Unmap all the attr blocks. */
	if (xfs_ifork_has_extents(sc->ip->i_afp)) {
		error = xrep_reap_fork(sc, sc->ip, XFS_ATTR_FORK);
		if (error)
			return error;
	}

	trace_xrep_xattr_reset_fork(sc->ip, sc->ip);

	error = xrep_xattr_fork_remove(sc, sc->ip);
	if (error)
		return error;

	return xfs_trans_roll_inode(&sc->tp, sc->ip);
}

/*
 * Find all the extended attributes for this inode by scraping them out of the
 * attribute key blocks by hand, and flushing them into the temp file.
 */
STATIC int
xrep_xattr_find_attributes(
	struct xrep_xattr	*rx)
{
	struct xfs_inode	*ip = rx->sc->ip;
	int			error;

	/* Short format xattrs are easy! */
	if (rx->sc->ip->i_afp->if_format == XFS_DINODE_FMT_LOCAL) {
		error = xrep_xattr_recover_sf(rx);
		if (error)
			return error;

		return xrep_xattr_flush_salvaged(rx);
	}

	/*
	 * Try to attach dquots, in case quotas were fixed since the setup
	 * function ran.
	 */
	error = xrep_ino_dqattach(rx->sc);
	if (error)
		return error;

	/*
	 * For non-inline xattr structures, the salvage function scans the
	 * buffer cache looking for potential attr leaf blocks.  The scan
	 * requires the ability to lock any buffer found and runs independently
	 * of any transaction <-> buffer item <-> buffer linkage.  Therefore,
	 * roll the transaction to ensure there are no buffers joined.  We hold
	 * the ILOCK independently of the transaction.
	 */
	error = xfs_trans_roll(&rx->sc->tp);
	if (error)
		return error;

	error = xfs_iread_extents(rx->sc->tp, ip, XFS_ATTR_FORK);
	if (error)
		return error;

	error = xrep_xattr_recover(rx);
	if (error)
		return error;

	return xrep_xattr_flush_salvaged(rx);
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

		ifp = XFS_IFORK_PTR(sc->ip, XFS_ATTR_FORK);

		xfs_idestroy_fork(ifp);
		ifp->if_format = XFS_DINODE_FMT_EXTENTS;
		ifp->if_nextents = 0;
		ifp->if_bytes = 0;
		ifp->if_u1.if_root = NULL;
		ifp->if_height = 0;

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

		if (!xfs_bmap_is_written_extent(&map))
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
		if (!xfs_bmap_is_written_extent(&map)) {
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

/* Swap the temporary file's attribute fork with the one being repaired. */
STATIC int
xrep_xattr_swap(
	struct xrep_xattr	*rx)
{
	struct xfs_scrub	*sc = rx->sc;
	bool			ip_local, temp_local;
	int			error = 0;

	/*
	 * Take the IOLOCK on the temporary file so that we can run xattr
	 * operations with the same locks held as we would for a normal file.
	 */
	while (!xrep_tempfile_iolock_nowait(rx->sc)) {
		if (xchk_should_terminate(rx->sc, &error))
			return error;
		delay(1);
	}

	error = xrep_tempswap_trans_alloc(rx->sc, XFS_ATTR_FORK, &rx->tx);
	if (error)
		return error;

	ip_local = sc->ip->i_afp->if_format == XFS_DINODE_FMT_LOCAL;
	temp_local = sc->tempip->i_afp->if_format == XFS_DINODE_FMT_LOCAL;

	/*
	 * If the both files have a local format attr fork and the rebuilt
	 * xattr data would fit in the repaired file's attr fork, just copy
	 * the contents from the tempfile and declare ourselves done.
	 */
	if (ip_local && temp_local) {
		int	forkoff;
		int	newsize;

		newsize = xfs_attr_sf_totsize(sc->tempip);
		forkoff = xfs_attr_shortform_bytesfit(sc->ip, newsize);
		if (forkoff > 0) {
			sc->ip->i_forkoff = forkoff;
			xrep_tempfile_copyout_local(sc, XFS_ATTR_FORK);
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

	return xrep_tempswap_contents(sc, &rx->tx);
}

/*
 * Swap the new extended attribute data (which we created in the tempfile) into
 * the file being repaired.
 */
STATIC int
xrep_xattr_rebuild_tree(
	struct xrep_xattr	*rx)
{
	struct xfs_scrub	*sc = rx->sc;
	int			error;

	/*
	 * If we didn't find any attributes to salvage, repair the file by
	 * zapping the attr fork.
	 */
	if (rx->attrs_found == 0) {
		xfs_trans_ijoin(sc->tp, sc->ip, 0);
		return xrep_xattr_reset_fork(sc);
	}

	trace_xrep_xattr_rebuild_tree(sc->ip, sc->tempip);

	/*
	 * Commit the repair transaction and drop the ILOCKs so that we can use
	 * the atomic extent swap helper functions to compute the correct
	 * resource reservations.
	 *
	 * We still hold IOLOCK_EXCL (aka i_rwsem) which will prevent xattr
	 * modifications, but there's nothing to prevent userspace from reading
	 * the attributes until we're ready for the swap operation.  Reads will
	 * return -EIO without shutting down the fs, so we're ok with that.
	 */
	error = xrep_trans_commit(sc);
	if (error)
		return error;

	xchk_iunlock(sc, XFS_ILOCK_EXCL);

	/*
	 * Swap the tempfile's attr fork with the file being repaired.  This
	 * recreates the transaction and takes the ILOCKs of both the file
	 * being repaired and the temporary file.
	 */
	error = xrep_xattr_swap(rx);
	if (error)
		return error;

	/*
	 * Wipe out the attr fork of the temp file so that regular inode
	 * inactivation won't trip over the corrupt attr fork.
	 */
	if (xfs_ifork_has_extents(sc->tempip->i_afp)) {
		error = xrep_reap_fork(sc, sc->tempip, XFS_ATTR_FORK);
		if (error)
			return error;
	}

	trace_xrep_xattr_reset_fork(sc->ip, sc->tempip);

	error = xrep_xattr_fork_remove(sc, sc->tempip);
	if (error)
		return error;

	return xrep_tempfile_roll_trans(sc);
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
	struct xrep_xattr	*rx;
	int			max_len;
	int			error;

	if (!xfs_inode_hasattr(sc->ip))
		return -ENOENT;

	/* We require the rmapbt to rebuild anything. */
	if (!xfs_has_rmapbt(sc->mp))
		return -EOPNOTSUPP;

	rx = kzalloc(sizeof(struct xrep_xattr), XCHK_GFP_FLAGS);
	if (!rx)
		return -ENOMEM;
	rx->sc = sc;

	/*
	 * Make sure we have enough space to handle salvaging and spilling
	 * every possible local attr value, since we only realloc the buffer
	 * for remote values.
	 */
	max_len = xfs_attr_leaf_entsize_local_max(sc->mp->m_attr_geo->blksize);
	error = xrep_setup_xattr_buf(sc, max_len, false);
	if (error == -ENOMEM)
		error = -EDEADLOCK;
	if (error)
		goto out_rx;

	/* Set up some storage */
	error = xfarray_create(sc->mp, "xattr keys", 0,
			sizeof(struct xrep_xattr_key), &rx->xattr_records);
	if (error)
		goto out_rx;

	error = xfblob_create(sc->mp, "xattr values", &rx->xattr_blobs);
	if (error)
		goto out_keys;

	ASSERT(sc->ilock_flags & XFS_ILOCK_EXCL);

	/*
	 * Collect extended attributes by parsing raw blocks to salvage
	 * whatever we can into the tempfile.  When we're done, free the
	 * staging memory before swapping the xattr structures to reduce memory
	 * usage.
	 */
	error = xrep_xattr_find_attributes(rx);
	if (error)
		goto out_values;

	xfblob_destroy(rx->xattr_blobs);
	xfarray_destroy(rx->xattr_records);
	rx->xattr_blobs = NULL;
	rx->xattr_records = NULL;

	/* Last chance to abort before we start committing fixes. */
	if (xchk_should_terminate(sc, &error))
		goto out_rx;

	/* Swap in the good contents. */
	error = xrep_xattr_rebuild_tree(rx);

out_values:
	if (rx->xattr_blobs)
		xfblob_destroy(rx->xattr_blobs);
out_keys:
	if (rx->xattr_records)
		xfarray_destroy(rx->xattr_records);
out_rx:
	kfree(rx);
	return error;
}
