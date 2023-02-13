// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
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
#include "xfs_attr.h"
#include "xfs_parent.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/tempfile.h"
#include "scrub/iscan.h"
#include "scrub/readdir.h"
#include "scrub/listxattr.h"
#include "scrub/xfile.h"
#include "scrub/xfarray.h"
#include "scrub/xfblob.h"

/*
 * Directory Repairs
 * =================
 *
 * Reconstruct a directory by visiting each parent pointer of each file in the
 * filesystem and translating the relevant pptrs into dirents.  Translation
 * occurs by adding new dirents to a temporary directory, which formats the
 * ondisk directory blocks.  In the final version of this code, we'll use the
 * atomic extent swap code to exchange the entire directory structure of the
 * file being repaired and the temporary, but for this PoC we omit the commit
 * to reduce the amount of code that has to be ported.
 *
 * Because we have to scan the entire filesystem, the next patch introduces the
 * inode scan and live update hooks so that the rebuilder can be kept aware of
 * filesystem updates being made to this directory by other threads.  Directory
 * entry translation therefore requires two steps to avoid problems with lock
 * contention and to keep ondisk tempdir updates out of the hook path.
 *
 * Every time the filesystem scanner or the live update hook code encounter a
 * directory operation relevant to this rebuilder, they will write a record of
 * the createname/removename operation to an xfarray.  Dirent names are stored
 * in an xfblob structure.  At opportune times, these stashed updates will be
 * read from the xfarray and committed (individually) to the temporary
 * directory.
 *
 * When the filesystem scan is complete, we relock both the directory and the
 * tempdir, and finish any stashed operations.  At that point, we are
 * theoretically ready to exchange the directory data fork mappings.  This
 * cannot happen until two patchsets get merged: the first allows callers to
 * specify the owning inode number explicitly; and the second is the atomic
 * extent swap series.
 *
 * For now we'll simply compare the two directories and complain about
 * discrepancies.
 */

/* Maximum memory usage for the tempdir log, in bytes. */
#define MAX_DIRENT_STASH_SIZE	(32ULL << 10)

/* Create a dirent in the tempdir. */
#define XREP_DIRENT_ADD		(1)

/* Remove a dirent from the tempdir. */
#define XREP_DIRENT_REMOVE	(2)

/* A stashed dirent update. */
struct xrep_dirent {
	/* Cookie for retrieval of the dirent name. */
	xfblob_cookie		name_cookie;

	/* Child inode number. */
	xfs_ino_t		ino;

	/* Length of the dirent name. */
	uint8_t			namelen;

	/* File type of the dirent. */
	uint8_t			ftype;

	/* XREP_DIRENT_{ADD,REMOVE} */
	uint8_t			action;
};

struct xrep_dir {
	struct xfs_scrub	*sc;

	/* Inode scan cursor. */
	struct xchk_iscan	iscan;

	/* Preallocated args struct for performing dir operations */
	struct xfs_da_args	args;

	/* Stashed directory entry updates. */
	struct xfarray		*dir_entries;

	/* Directory entry names. */
	struct xfblob		*dir_names;

	/* Mutex protecting dir_entries, dir_names, and parent_ino. */
	struct mutex		lock;

	/* Hook to capture directory entry updates. */
	struct xfs_dirent_hook	hooks;

	/*
	 * This is the dotdot inumber that we're going to set on the
	 * reconstructed directory.
	 */
	xfs_ino_t		parent_ino;

	/* Scratch buffer for scanning pptr xattrs */
	struct xfs_parent_name_irec pptr;
};

/* Tear down all the incore stuff we created. */
static void
xrep_dir_teardown(
	struct xfs_scrub	*sc)
{
	struct xrep_dir		*rd = sc->buf;

	xfs_dirent_hook_del(sc->mp, &rd->hooks);
	xchk_iscan_finish(&rd->iscan);
	mutex_destroy(&rd->lock);
	xfblob_destroy(rd->dir_names);
	xfarray_destroy(rd->dir_entries);
}

/* Set up for a directory repair. */
int
xrep_setup_directory(
	struct xfs_scrub	*sc)
{
	struct xrep_dir		*rd;
	int			error;

	xchk_fshooks_enable(sc, XCHK_FSHOOKS_DIRENTS);

	error = xrep_tempfile_create(sc, S_IFDIR);
	if (error)
		return error;

	rd = kvzalloc(sizeof(struct xrep_dir), XCHK_GFP_FLAGS);
	if (!rd)
		return -ENOMEM;

	sc->buf = rd;
	rd->sc = sc;
	rd->parent_ino = NULLFSINO;
	return 0;
}

/* Are these two directory names the same? */
static inline bool
xrep_dir_samename(
	const struct xfs_name	*n1,
	const struct xfs_name	*n2)
{
	return n1->len == n2->len && !memcmp(n1->name, n2->name, n1->len);
}

/*
 * Look up the inode number for an exact name in a directory.
 *
 * Callers must hold the ILOCK.  File types are XFS_DIR3_FT_*.  Names are not
 * checked for correctness.  This initializes rd->args.
 */
STATIC int
xrep_dir_lookup(
	struct xrep_dir		*rd,
	struct xfs_inode	*dp,
	const struct xfs_name	*name,
	xfs_ino_t		*ino)
{
	struct xfs_scrub	*sc = rd->sc;
	bool			isblock, isleaf;
	int			error;

	if (xfs_is_shutdown(dp->i_mount))
		return -EIO;

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));
	ASSERT(xfs_isilocked(dp, XFS_ILOCK_SHARED | XFS_ILOCK_EXCL));

	memset(&rd->args, 0, sizeof(struct xfs_da_args));
	rd->args.dp		= dp;
	rd->args.geo		= sc->mp->m_dir_geo;
	rd->args.hashval	= xfs_dir2_hashname(dp->i_mount, name);
	rd->args.namelen	= name->len;
	rd->args.name		= name->name;
	rd->args.op_flags	= XFS_DA_OP_OKNOENT;
	rd->args.trans		= sc->tp;
	rd->args.whichfork	= XFS_DATA_FORK;

	if (dp->i_df.if_format == XFS_DINODE_FMT_LOCAL) {
		error = xfs_dir2_sf_lookup(&rd->args);
		goto out_check_rval;
	}

	/* dir2 functions require that the data fork is loaded */
	error = xfs_iread_extents(sc->tp, dp, XFS_DATA_FORK);
	if (error)
		return error;

	error = xfs_dir2_isblock(&rd->args, &isblock);
	if (error)
		return error;

	if (isblock) {
		error = xfs_dir2_block_lookup(&rd->args);
		goto out_check_rval;
	}

	error = xfs_dir2_isleaf(&rd->args, &isleaf);
	if (error)
		return error;

	if (isleaf) {
		error = xfs_dir2_leaf_lookup(&rd->args);
		goto out_check_rval;
	}

	error = xfs_dir2_node_lookup(&rd->args);

out_check_rval:
	if (error == -EEXIST)
		error = 0;
	if (!error)
		*ino = rd->args.inumber;
	return error;
}

/* Create a directory entry, having filled out most of rd->args via lookup. */
STATIC int
xrep_dir_createname(
	struct xrep_dir		*rd,
	const struct xfs_name	*name,
	xfs_ino_t		inum,
	xfs_extlen_t		total)
{
	struct xfs_scrub	*sc = rd->sc;
	struct xfs_inode	*dp = rd->args.dp;
	bool			is_block, is_leaf;
	int			error;

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));

	error = xfs_dir_ino_validate(sc->mp, inum);
	if (error)
		return error;

	trace_xrep_dir_createname(dp, name, inum);

	/* reset cmpresult as if we haven't done a lookup */
	rd->args.cmpresult = XFS_CMP_DIFFERENT;
	rd->args.filetype = name->type;
	rd->args.inumber = inum;
	rd->args.op_flags = XFS_DA_OP_ADDNAME | XFS_DA_OP_OKNOENT;
	rd->args.total = total;

	if (dp->i_df.if_format == XFS_DINODE_FMT_LOCAL)
		return xfs_dir2_sf_addname(&rd->args);

	error = xfs_dir2_isblock(&rd->args, &is_block);
	if (error)
		return error;
	if (is_block)
		return xfs_dir2_block_addname(&rd->args);

	error = xfs_dir2_isleaf(&rd->args, &is_leaf);
	if (error)
		return error;
	if (is_leaf)
		return xfs_dir2_leaf_addname(&rd->args);

	return xfs_dir2_node_addname(&rd->args);
}

/* Remove a directory entry, having filled out rd->args via lookup. */
STATIC int
xrep_dir_removename(
	struct xrep_dir		*rd,
	const struct xfs_name	*name,
	xfs_extlen_t		total)
{
	struct xfs_inode	*dp = rd->args.dp;
	bool			is_block, is_leaf;
	int			error;

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));

	/* reset cmpresult as if we haven't done a lookup */
	rd->args.cmpresult = XFS_CMP_DIFFERENT;
	rd->args.op_flags = 0;
	rd->args.total = total;

	trace_xrep_dir_removename(dp, name, rd->args.inumber);

	if (dp->i_df.if_format == XFS_DINODE_FMT_LOCAL)
		return xfs_dir2_sf_removename(&rd->args);

	error = xfs_dir2_isblock(&rd->args, &is_block);
	if (error)
		return error;
	if (is_block)
		return xfs_dir2_block_removename(&rd->args);

	error = xfs_dir2_isleaf(&rd->args, &is_leaf);
	if (error)
		return error;
	if (is_leaf)
		return xfs_dir2_leaf_removename(&rd->args);

	return xfs_dir2_node_removename(&rd->args);
}

/* Update the temporary directory with a stashed update. */
STATIC int
xrep_dir_replay_update(
	struct xrep_dir			*rd,
	const struct xrep_dirent	*dirent)
{
	struct xfs_name			xname = {
		.len			= dirent->namelen,
		.type			= dirent->ftype,
		.name			= rd->pptr.p_name,
	};
	struct xfs_scrub		*sc = rd->sc;
	struct xfs_mount		*mp = sc->mp;
	xfs_ino_t			child_ino;
	uint				resblks;
	int				error;

	if (dirent->action == XREP_DIRENT_REMOVE)
		resblks = XFS_DIRREMOVE_SPACE_RES(mp);
	else
		resblks = XFS_DIRENTER_SPACE_RES(mp, dirent->namelen);

	error = xchk_trans_alloc(sc, resblks);
	if (error)
		return error;

	error = xrep_tempfile_ilock_polled(sc);
	if (error) {
		xchk_trans_cancel(rd->sc);
		return error;
	}

	xfs_trans_ijoin(sc->tp, sc->tempip, 0);

	error = xrep_dir_lookup(rd, sc->tempip, &xname, &child_ino);
	if (dirent->action == XREP_DIRENT_REMOVE) {
		/* Remove this dirent.  The lookup must succeed. */
		if (error)
			goto out_cancel;
		if (child_ino != dirent->ino) {
			error = -ENOENT;
			goto out_cancel;
		}

		error = xrep_dir_removename(rd, &xname, resblks);
	} else {
		/* Add this dirent.  The lookup must not succeed. */
		if (error == 0)
			error = -EEXIST;
		if (error != -ENOENT)
			goto out_cancel;

		error = xrep_dir_createname(rd, &xname, dirent->ino, resblks);
	}
	if (error)
		goto out_cancel;

	error = xrep_trans_commit(sc);
	goto out_ilock;

out_cancel:
	xchk_trans_cancel(rd->sc);
out_ilock:
	xrep_tempfile_iunlock(rd->sc);
	return error;
}

/*
 * Flush stashed dirent updates that have been recorded by the scanner.  This
 * is done to reduce the memory requirements of the directory rebuild, since
 * directories can contain up to 32GB of directory data.
 *
 * Caller must not hold transactions or ILOCKs.  Caller must hold the tempdir
 * IOLOCK.
 */
STATIC int
xrep_dir_replay_updates(
	struct xrep_dir		*rd)
{
	xfarray_idx_t		array_cur;
	int			error;

	mutex_lock(&rd->lock);
	foreach_xfarray_idx(rd->dir_entries, array_cur) {
		struct xrep_dirent	dirent;

		error = xfarray_load(rd->dir_entries, array_cur, &dirent);
		if (error)
			goto out_unlock;

		error = xfblob_load(rd->dir_names, dirent.name_cookie,
				rd->pptr.p_name, dirent.namelen);
		if (error)
			goto out_unlock;
		rd->pptr.p_name[MAXNAMELEN - 1] = 0;
		mutex_unlock(&rd->lock);

		error = xrep_dir_replay_update(rd, &dirent);
		if (error)
			return error;

		mutex_lock(&rd->lock);
	}

	/* Empty out both arrays now that we've added the entries. */
	xfarray_truncate(rd->dir_entries);
	xfblob_truncate(rd->dir_names);
	mutex_unlock(&rd->lock);
	return 0;
out_unlock:
	mutex_unlock(&rd->lock);
	return error;
}

/*
 * Remember that we want to create a dirent in the tempdir.  These stashed
 * actions will be replayed later.
 */
STATIC int
xrep_dir_add_dirent(
	struct xrep_dir		*rd,
	const struct xfs_name	*name,
	xfs_ino_t		ino)
{
	struct xrep_dirent	dirent = {
		.action		= XREP_DIRENT_ADD,
		.ino		= ino,
		.namelen	= name->len,
		.ftype		= name->type,
	};
	int			error;

	trace_xrep_dir_add_dirent(rd->sc->tempip, name, ino);

	error = xfblob_store(rd->dir_names, &dirent.name_cookie, name->name,
			name->len);
	if (error)
		return error;

	return xfarray_append(rd->dir_entries, &dirent);
}

/*
 * Remember that we want to remove a dirent from the tempdir.  These stashed
 * actions will be replayed later.
 */
STATIC int
xrep_dir_remove_dirent(
	struct xrep_dir		*rd,
	const struct xfs_name	*name,
	xfs_ino_t		ino)
{
	struct xrep_dirent	dirent = {
		.action		= XREP_DIRENT_REMOVE,
		.ino		= ino,
		.namelen	= name->len,
		.ftype		= name->type,
	};
	int			error;

	trace_xrep_dir_remove_dirent(rd->sc->tempip, name, ino);

	error = xfblob_store(rd->dir_names, &dirent.name_cookie, name->name,
			name->len);
	if (error)
		return error;

	return xfarray_append(rd->dir_entries, &dirent);
}

/*
 * Examine an xattr of a file.  If this xattr is a parent pointer that leads us
 * back to the directory that we're rebuilding, add a dirent to the temporary
 * directory.
 */
STATIC int
xrep_dir_scan_parent_pointer(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip,
	unsigned int		attr_flags,
	const unsigned char	*name,
	unsigned int		namelen,
	const void		*value,
	unsigned int		valuelen,
	void			*priv)
{
	struct xfs_name		xname;
	struct xrep_dir		*rd = priv;
	const struct xfs_parent_name_rec *rec = (const void *)name;
	int			error;

	/* Ignore incomplete xattrs */
	if (attr_flags & XFS_ATTR_INCOMPLETE)
		return 0;

	/* Ignore anything that isn't a parent pointer. */
	if (!(attr_flags & XFS_ATTR_PARENT))
		return 0;

	/* Does the ondisk parent pointer structure make sense? */
	if (!xfs_parent_namecheck(sc->mp, rec, namelen, attr_flags) ||
	    !xfs_parent_valuecheck(sc->mp, namelen, value, valuelen))
		return -EFSCORRUPTED;

	xfs_parent_irec_from_disk(&rd->pptr, rec, namelen, value, valuelen);

	/* Ignore parent pointers that point back to a different dir. */
	if (rd->pptr.p_ino != sc->ip->i_ino ||
	    rd->pptr.p_gen != VFS_I(sc->ip)->i_generation)
		return 0;

	/*
	 * Transform this parent pointer into a dirent and queue it for later
	 * addition to the temporary directory.
	 */
	xname.name = rd->pptr.p_name;
	xname.len = rd->pptr.p_namelen;
	xname.type = xfs_mode_to_ftype(VFS_I(ip)->i_mode);

	mutex_lock(&rd->lock);
	error = xrep_dir_add_dirent(rd, &xname, ip->i_ino);
	mutex_unlock(&rd->lock);
	return error;
}

/*
 * If this child dirent points to the directory being repaired, remember that
 * fact so that we can reset the dotdot entry if necessary.
 */
STATIC int
xrep_dir_scan_dirent(
	struct xfs_scrub	*sc,
	struct xfs_inode	*dp,
	xfs_dir2_dataptr_t	dapos,
	const struct xfs_name	*name,
	xfs_ino_t		ino,
	void			*priv)
{
	struct xrep_dir		*rd = priv;

	/* Dirent doesn't point to this directory. */
	if (ino != rd->sc->ip->i_ino)
		return 0;

	/* Ignore garbage inum. */
	if (!xfs_verify_dir_ino(rd->sc->mp, ino))
		return 0;

	/* No weird looking names. */
	if (name->len >= MAXNAMELEN || name->len <= 0)
		return 0;

	/* Don't pick up dot or dotdot entries; we only want child dirents. */
	if (xrep_dir_samename(name, &xfs_name_dotdot) ||
	    xrep_dir_samename(name, &xfs_name_dot))
		return 0;

	trace_xrep_dir_replacename(sc->tempip, &xfs_name_dotdot, dp->i_ino);

	mutex_lock(&rd->lock);
	rd->parent_ino = dp->i_ino;
	mutex_unlock(&rd->lock);
	return 0;
}

/*
 * Decide if we want to look for child dirents or parent pointers in this file.
 * Skip the dir being repaired and any files being used to stage repairs.
 */
static inline bool
xrep_dir_want_scan(
	struct xrep_dir		*rd,
	const struct xfs_inode	*ip)
{
	return ip != rd->sc->ip && !xrep_is_tempfile(ip);
}

/*
 * Take ILOCK on a file that we want to scan.
 *
 * Select ILOCK_EXCL if the file is a directory with an unloaded data bmbt or
 * has an unloaded attr bmbt.  Otherwise, take ILOCK_SHARED.
 */
static inline unsigned int
xrep_dir_scan_ilock(
	struct xrep_dir		*rd,
	struct xfs_inode	*ip)
{
	uint			lock_mode = XFS_ILOCK_SHARED;

	/* Need to take the shared ILOCK to advance the iscan cursor. */
	if (!xrep_dir_want_scan(rd, ip))
		goto lock;

	if (S_ISDIR(VFS_I(ip)->i_mode) && xfs_need_iread_extents(&ip->i_df)) {
		lock_mode = XFS_ILOCK_EXCL;
		goto lock;
	}

	if (xfs_inode_has_attr_fork(ip) && xfs_need_iread_extents(&ip->i_af))
		lock_mode = XFS_ILOCK_EXCL;

lock:
	xfs_ilock(ip, lock_mode);
	return lock_mode;
}

/*
 * Scan this file for relevant child dirents or parent pointers that point to
 * the directory we're rebuilding.
 */
STATIC int
xrep_dir_scan_file(
	struct xrep_dir		*rd,
	struct xfs_inode	*ip)
{
	unsigned int		lock_mode;
	int			error = 0;

	lock_mode = xrep_dir_scan_ilock(rd, ip);

	if (!xrep_dir_want_scan(rd, ip))
		goto scan_done;

	error = xchk_xattr_walk(rd->sc, ip, xrep_dir_scan_parent_pointer, rd);
	if (error)
		goto scan_done;

	if (S_ISDIR(VFS_I(ip)->i_mode)) {
		error = xchk_dir_walk(rd->sc, ip, xrep_dir_scan_dirent, rd);
		if (error)
			goto scan_done;
	}

scan_done:
	xchk_iscan_mark_visited(&rd->iscan, ip);
	xfs_iunlock(ip, lock_mode);
	return error;
}

/* Scan all files in the filesystem for dirents. */
STATIC int
xrep_dir_scan_dirtree(
	struct xrep_dir		*rd)
{
	struct xfs_scrub	*sc = rd->sc;
	struct xfs_inode	*ip;
	int			error;

	/*
	 * Filesystem scans are time consuming.  Drop the directory ILOCK and
	 * all other resources for the duration of the scan and hope for the
	 * best.
	 */
	xchk_trans_cancel(sc);
	if (sc->ilock_flags & (XFS_ILOCK_SHARED | XFS_ILOCK_EXCL))
		xchk_iunlock(sc, sc->ilock_flags & (XFS_ILOCK_SHARED |
						    XFS_ILOCK_EXCL));
	error = xchk_trans_alloc_empty(sc);
	if (error)
		return error;

	while ((error = xchk_iscan_iter(sc, &rd->iscan, &ip)) == 1) {
		uint64_t	mem_usage;

		error = xrep_dir_scan_file(rd, ip);
		xchk_irele(sc, ip);
		if (error)
			break;

		/* Flush stashed dirent updates to constrain memory usage. */
		mutex_lock(&rd->lock);
		mem_usage = xfarray_bytes(rd->dir_entries) +
			     xfblob_bytes(rd->dir_names);
		mutex_unlock(&rd->lock);
		if (mem_usage >= MAX_DIRENT_STASH_SIZE) {
			xchk_trans_cancel(sc);

			error = xrep_tempfile_iolock_polled(sc);
			if (error)
				break;

			error = xrep_dir_replay_updates(rd);
			xrep_tempfile_iounlock(sc);
			if (error)
				break;

			error = xchk_trans_alloc_empty(sc);
			if (error)
				break;
		}

		if (xchk_should_terminate(sc, &error))
			break;
	}
	if (error) {
		/*
		 * If we couldn't grab an inode that was busy with a state
		 * change, change the error code so that we exit to userspace
		 * as quickly as possible.
		 */
		if (error == -EBUSY)
			return -ECANCELED;
		return error;
	}

	return 0;
}

/*
 * Dump a dirent from the temporary dir and check it against the dir we're
 * rebuilding.  We are not committing any of this.
 */
STATIC int
xrep_dir_dump_tempdir(
	struct xfs_scrub	*sc,
	struct xfs_inode	*dp,
	xfs_dir2_dataptr_t	dapos,
	const struct xfs_name	*name,
	xfs_ino_t		ino,
	void			*priv)
{
	struct xrep_dir		*rd = priv;
	xfs_ino_t		child_ino;
	bool			child_dirent = true;
	bool			compare_dirent = true;
	int			error;

	/*
	 * The tempdir was created with a dotdot entry pointing to the root
	 * directory.  Substitute whatever inode number we found during the
	 * filesystem scan.
	 *
	 * The tempdir was also created with a dot entry pointing to itself.
	 * Substitute the inode number of the directory being repaired.  A
	 * prerequisite for the real repair code is a patchset to allow dir
	 * callers to set the owner (and dot entry in the case of sf -> block
	 * conversion) explicitly.
	 *
	 * I've chosen not to port the owner setting patchset or the swapext
	 * patchset for this PoC, which is why we build the tempdir, compare
	 * the contents, and drop the tempdir.
	 */
	if (xrep_dir_samename(name, &xfs_name_dotdot)) {
		child_dirent = false;
		ino = rd->parent_ino;
	}
	if (xrep_dir_samename(name, &xfs_name_dot)) {
		child_dirent = false;
		compare_dirent = false;
		ino = sc->ip->i_ino;
	}

	trace_xrep_dir_dumpname(sc->tempip, name, ino);

	/* Check that the dir being repaired has the same entry. */
	if (compare_dirent) {
		error = xchk_dir_lookup(sc, sc->ip, name, &child_ino, NULL);
		if (error == -ENOENT) {
			trace_xrep_dir_checkname(sc->ip, name, NULLFSINO);
			ASSERT(error != -ENOENT);
			return -EFSCORRUPTED;
		}
		if (error)
			return error;

		if (ino != child_ino) {
			trace_xrep_dir_checkname(sc->ip, name, child_ino);
			ASSERT(ino == child_ino);
			return -EFSCORRUPTED;
		}
	}

	/*
	 * Set ourselves up to free every dirent in the tempdir because
	 * directory inactivation won't do it for us.  The rest of the online
	 * fsck patchset provides us a means to swap the directory structure
	 * and reap it responsibly, but I didn't feel like porting all that.
	 */
	if (child_dirent) {
		mutex_lock(&rd->lock);
		error = xrep_dir_remove_dirent(rd, name, ino);
		mutex_unlock(&rd->lock);
	}

	return error;
}

/*
 * Dump a dirent from the dir we're rebuilding and check it against the
 * temporary dir.  This assumes that the directory wasn't really corrupt to
 * begin with.
 */
STATIC int
xrep_dir_dump_baddir(
	struct xfs_scrub	*sc,
	struct xfs_inode	*dp,
	xfs_dir2_dataptr_t	dapos,
	const struct xfs_name	*name,
	xfs_ino_t		ino,
	void			*priv)
{
	xfs_ino_t		child_ino;
	int			error;

	/* Ignore the directory's dot and dotdot entries. */
	if (xrep_dir_samename(name, &xfs_name_dotdot) ||
	    xrep_dir_samename(name, &xfs_name_dot))
		return 0;

	trace_xrep_dir_dumpname(sc->ip, name, ino);

	/* Check that the tempdir has the same entry. */
	error = xchk_dir_lookup(sc, sc->tempip, name, &child_ino, NULL);
	if (error == -ENOENT) {
		trace_xrep_dir_checkname(sc->tempip, name, NULLFSINO);
		ASSERT(error != -ENOENT);
		return -EFSCORRUPTED;
	}
	if (error)
		return error;

	if (ino != child_ino) {
		trace_xrep_dir_checkname(sc->tempip, name, child_ino);
		ASSERT(ino == child_ino);
		return -EFSCORRUPTED;
	}

	return 0;
}

/*
 * "Commit" the new directory structure to the file that we're repairing.
 *
 * In the final version, we'd swap the new directory contents (which we created
 * in the tempfile) into the directory being repaired.  For now we just lock
 * the temporary dir and dump what we found.
 */
STATIC int
xrep_dir_rebuild_tree(
	struct xrep_dir		*rd)
{
	struct xfs_scrub	*sc = rd->sc;
	int			error = 0;

	/*
	 * Replay the last of the stashed dirent updates.  We still hold the
	 * IOLOCK_EXCL of the directory that we're repairing and the temporary
	 * directory.
	 */
	xchk_trans_cancel(sc);

	ASSERT(sc->ilock_flags & XFS_IOLOCK_EXCL);
	error = xrep_tempfile_iolock_polled(sc);
	if (error)
		return error;

	/*
	 * Replay stashed updates and take the ILOCKs of both directories
	 * before we simulate committing the new directory structure.
	 *
	 * As of Linux 6.2, if /a, /a/b, and /c are all directories, the VFS
	 * does not take i_rwsem on /a/b for a "mv /a/b /c/" operation.  This
	 * means that only b's ILOCK protects b's dotdot update.  b's IOLOCK
	 * is not held, unlike every other dotdot update.  To stabilize sc->ip
	 * to simulate the repair commit, we must hold the ILOCK of the
	 * directory being repaired /and/ there must not be any pending live
	 * updates.
	 */
	do {
		error = xrep_dir_replay_updates(rd);
		if (error)
			return error;

		error = xchk_trans_alloc_empty(sc);
		if (error)
			return error;

		xchk_ilock(sc, XFS_ILOCK_EXCL);
		if (xfarray_length(rd->dir_entries) == 0)
			break;

		xchk_iunlock(sc, XFS_ILOCK_EXCL);
		xchk_trans_cancel(sc);
	} while (!xchk_should_terminate(sc, &error));
	if (error)
		return error;

	if (sc->ip == sc->mp->m_rootip) {
		/* Should not have found any parent of the root directory. */
		ASSERT(rd->parent_ino == NULLFSINO);
		rd->parent_ino = sc->mp->m_rootip->i_ino;
	} else if (rd->parent_ino == NULLFSINO) {
		/*
		 * Should have found a parent somewhere unless this is an
		 * unlinked directory.
		 */
		ASSERT(VFS_I(sc->ip)->i_nlink == 0);
		rd->parent_ino = rd->sc->mp->m_sb.sb_rootino;
	}

	trace_xrep_dir_rebuild_tree(sc->ip, rd->parent_ino);

	/*
	 * At this point, we've quiesced both directories and should be ready
	 * to commit the new contents.
	 *
	 * We don't have atomic swapext here, so all we do is dump the dirents
	 * that we found to the ftrace buffer and {ab,re}use the dirent update
	 * stashing mechanism to schedule deletion of every dirent in the
	 * temporary directory to avoid leaking directory blocks.
	 */
	error = xrep_tempfile_ilock_polled(sc);
	if (error)
		return error;

	error = xchk_dir_walk(sc, sc->tempip, xrep_dir_dump_tempdir, rd);
	if (error)
		return error;

	error = xchk_dir_walk(sc, sc->ip, xrep_dir_dump_baddir, rd);
	if (error)
		return error;

	xrep_tempfile_iunlock(sc);
	xchk_iunlock(sc, XFS_ILOCK_EXCL);
	xchk_trans_cancel(sc);

	return xrep_dir_replay_updates(rd);
}

/*
 * Capture dirent updates being made by other threads which are relevant to the
 * directory being repaired.
 */
STATIC int
xrep_dir_live_update(
	struct notifier_block		*nb,
	unsigned long			action,
	void				*data)
{
	struct xfs_dirent_update_params	*p = data;
	struct xrep_dir			*rd;
	struct xfs_scrub		*sc;
	int				error = 0;

	rd = container_of(nb, struct xrep_dir, hooks.delta_hook.nb);
	sc = rd->sc;

	/*
	 * This thread updated a child dirent in the directory that we're
	 * rebuilding.  Stash the update for replay against the temporary
	 * directory.
	 */
	if (action == XFS_DIRENT_CHILD_DELTA &&
	    p->dp->i_ino == sc->ip->i_ino &&
	    xchk_iscan_want_live_update(&rd->iscan, p->ip->i_ino)) {
		mutex_lock(&rd->lock);
		if (p->delta > 0)
			error = xrep_dir_add_dirent(rd, p->name, p->ip->i_ino);
		else
			error = xrep_dir_remove_dirent(rd, p->name,
					p->ip->i_ino);
		mutex_unlock(&rd->lock);
		if (error)
			goto out_abort;
	}

	/*
	 * This thread updated another directory's child dirent that points to
	 * the directory that we're rebuilding, so remember the new dotdot
	 * target.
	 */
	if (action == XFS_DIRENT_BACKREF_DELTA &&
	    p->ip->i_ino == sc->ip->i_ino &&
	    xchk_iscan_want_live_update(&rd->iscan, p->dp->i_ino)) {
		if (p->delta > 0) {
			trace_xrep_dir_add_dirent(sc->tempip, &xfs_name_dotdot,
					p->dp->i_ino);

			mutex_lock(&rd->lock);
			rd->parent_ino = p->dp->i_ino;
			mutex_unlock(&rd->lock);
		} else {
			trace_xrep_dir_remove_dirent(sc->tempip,
					&xfs_name_dotdot, NULLFSINO);

			mutex_lock(&rd->lock);
			rd->parent_ino = NULLFSINO;
			mutex_unlock(&rd->lock);
		}
	}

	return NOTIFY_DONE;
out_abort:
	xchk_iscan_abort(&rd->iscan);
	return NOTIFY_DONE;
}

/* Set up the filesystem scan so we can regenerate directory entries. */
STATIC int
xrep_dir_setup_scan(
	struct xrep_dir		*rd)
{
	struct xfs_scrub	*sc = rd->sc;
	int			error;

	error = xfarray_create(sc->mp, "directory entries", 0,
			sizeof(struct xrep_dirent), &rd->dir_entries);
	if (error)
		return error;

	error = xfblob_create(sc->mp, "dirent names", &rd->dir_names);
	if (error)
		goto out_entries;

	mutex_init(&rd->lock);

	/* Retry iget every tenth of a second for up to 30 seconds. */
	xchk_iscan_start(&rd->iscan, 30000, 100);

	/*
	 * Hook into the dirent update code.  The hook only operates on inodes
	 * that were already scanned, and the scanner thread takes each inode's
	 * ILOCK, which means that any in-progress inode updates will finish
	 * before we can scan the inode.
	 */
	ASSERT(sc->flags & XCHK_FSHOOKS_DIRENTS);
	xfs_hook_setup(&rd->hooks.delta_hook, xrep_dir_live_update);
	error = xfs_dirent_hook_add(sc->mp, &rd->hooks);
	if (error)
		goto out_scan;

	return 0;

out_scan:
	xchk_iscan_finish(&rd->iscan);
	mutex_destroy(&rd->lock);
	xfblob_destroy(rd->dir_names);
out_entries:
	xfarray_destroy(rd->dir_entries);
	return error;
}

/*
 * Repair the directory metadata.
 *
 * XXX: Is it necessary to check the dcache for this directory to make sure
 * that we always recreate every cached entry?
 */
int
xrep_directory(
	struct xfs_scrub	*sc)
{
	struct xrep_dir		*rd = sc->buf;
	int			error = 0;

	/* We require directory parent pointers to rebuild anything. */
	if (!xfs_has_parent(sc->mp))
		return -EOPNOTSUPP;

	error = xrep_dir_setup_scan(rd);
	if (error)
		goto out;

	error = xrep_dir_scan_dirtree(rd);
	if (error)
		goto out_finish_scan;

	error = xrep_dir_rebuild_tree(rd);
	if (error)
		goto out_finish_scan;

out_finish_scan:
	xrep_dir_teardown(sc);
out:
	return error;
}
