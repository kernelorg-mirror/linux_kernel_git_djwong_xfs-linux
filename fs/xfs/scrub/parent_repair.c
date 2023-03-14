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
 * Parent Pointer Repairs
 * ======================
 *
 * Reconstruct a file's parent pointers by visiting each dirent of each
 * directory in the filesystem and translating the relevant dirents into parent
 * pointers.  Translation occurs by adding new parent pointers to a temporary
 * file, which formats the ondisk extended attribute blocks.  In the final
 * version of this code, we'll use the atomic extent swap code to exchange the
 * entire xattr structure of the file being repaired and the temporary file,
 * but for this PoC we omit the commit to reduce the amount of code that has to
 * be ported.
 *
 * Because we have to scan the entire filesystem, the next patch introduces the
 * inode scan and live update hooks so that the rebuilder can be kept aware of
 * filesystem updates being made to this file's parents by other threads.
 * Parent pointer translation therefore requires two steps to avoid problems
 * with lock contention and to keep ondisk tempdir updates out of the hook
 * path.
 *
 * Every time the filesystem scanner or the live update hook code encounter a
 * directory operation relevant to this rebuilder, they will write a record of
 * the createpptr/removepptr operation to an xfarray.  Parent pointer names are
 * stored in an xfblob structure.  At opportune times, these stashed updates
 * will be read from the xfarray and committed (individually) to the temporary
 * file's parent pointers.
 *
 * When the filesystem scan is complete, we relock both the file and the
 * tempfile, and finish any stashed operations.  At that point, had we copied
 * the extended attributes, we would be ready to exchange the attribute data
 * fork mappings.  This cannot happen until two patchsets get merged: the first
 * allows callers to specify the owning inode number explicitly; and the second
 * is the atomic extent swap series.
 *
 * For now we'll simply compare the two files parent pointers and complain
 * about discrepancies.
 */

/* Maximum memory usage for the tempfile log, in bytes. */
#define MAX_PPTR_STASH_SIZE	(32ULL << 10)

/* Create a parent pointer in the tempfile. */
#define XREP_PPTR_ADD		(1)

/* Remove a parent pointer from the tempfile. */
#define XREP_PPTR_REMOVE	(2)

/* A stashed parent pointer update. */
struct xrep_pptr {
	/* Cookie for retrieval of the pptr name. */
	xfblob_cookie			name_cookie;

	/* Parent pointer attr key. */
	xfs_ino_t			p_ino;
	uint32_t			p_gen;
	xfs_dir2_dataptr_t		p_diroffset;

	/* Length of the pptr name. */
	uint8_t				namelen;

	/* XREP_PPTR_{ADD,REMOVE} */
	uint8_t				action;
};

struct xrep_pptrs {
	struct xfs_scrub	*sc;

	/* Inode scan cursor. */
	struct xchk_iscan	iscan;

	/* Scratch buffer for scanning dirents to create pptr xattrs */
	struct xfs_parent_name_irec pptr;

	/* xattr key and da args for parent pointer replay. */
	struct xfs_parent_scratch pptr_scratch;

	/* Mutex protecting parent_ptrs, pptr_names. */
	struct mutex		lock;

	/* Stashed parent pointer updates. */
	struct xfarray		*parent_ptrs;

	/* Parent pointer names. */
	struct xfblob		*pptr_names;
};

/* Tear down all the incore stuff we created. */
static void
xrep_pptr_teardown(
	struct xrep_pptrs	*rp)
{
	xchk_iscan_finish(&rp->iscan);
	mutex_destroy(&rp->lock);
	xfblob_destroy(rp->pptr_names);
	xfarray_destroy(rp->parent_ptrs);
}

/* Set up for a parent pointer repair. */
int
xrep_setup_parent(
	struct xfs_scrub	*sc)
{
	struct xrep_pptrs	*rp;
	int			error;

	error = xrep_tempfile_create(sc, S_IFREG);
	if (error)
		return error;

	rp = kvzalloc(sizeof(struct xrep_pptrs), XCHK_GFP_FLAGS);
	if (!rp)
		return -ENOMEM;

	sc->buf = rp;
	rp->sc = sc;
	return 0;
}

/* Are these two parent pointer names the same? */
static inline bool
xrep_pptr_samename(
	const struct xfs_name	*n1,
	const struct xfs_name	*n2)
{
	return n1->len == n2->len && !memcmp(n1->name, n2->name, n1->len);
}

/* Update the temporary file's parent pointers with a stashed update. */
STATIC int
xrep_pptr_replay_update(
	struct xrep_pptrs	*rp,
	const struct xrep_pptr	*pptr)
{
	struct xfs_scrub	*sc = rp->sc;

	rp->pptr.p_ino = pptr->p_ino;
	rp->pptr.p_gen = pptr->p_gen;
	rp->pptr.p_diroffset = pptr->p_diroffset;
	rp->pptr.p_namelen = pptr->namelen;

	if (pptr->action == XREP_PPTR_ADD) {
		/* Create parent pointer. */
		trace_xrep_pptr_createname(sc->tempip, &rp->pptr);

		return xfs_parent_set(sc->tempip, &rp->pptr, &rp->pptr_scratch);
	}

	ASSERT(0);
	return -EOPNOTSUPP;
}

/*
 * Flush stashed parent pointer updates that have been recorded by the scanner.
 * This is done to reduce the memory requirements of the parent pointer
 * rebuild, since files can have a lot of hardlinks and the fs can be busy.
 *
 * Caller must not hold transactions or ILOCKs.  Caller must hold the tempfile
 * IOLOCK.
 */
STATIC int
xrep_pptr_replay_updates(
	struct xrep_pptrs	*rp)
{
	xfarray_idx_t		array_cur;
	int			error;

	mutex_lock(&rp->lock);
	foreach_xfarray_idx(rp->parent_ptrs, array_cur) {
		struct xrep_pptr	pptr;

		error = xfarray_load(rp->parent_ptrs, array_cur, &pptr);
		if (error)
			goto out_unlock;

		error = xfblob_load(rp->pptr_names, pptr.name_cookie,
				rp->pptr.p_name, pptr.namelen);
		if (error)
			goto out_unlock;
		rp->pptr.p_name[MAXNAMELEN - 1] = 0;
		mutex_unlock(&rp->lock);

		error = xrep_pptr_replay_update(rp, &pptr);
		if (error)
			return error;

		mutex_lock(&rp->lock);
	}

	/* Empty out both arrays now that we've added the entries. */
	xfarray_truncate(rp->parent_ptrs);
	xfblob_truncate(rp->pptr_names);
	mutex_unlock(&rp->lock);
	return 0;
out_unlock:
	mutex_unlock(&rp->lock);
	return error;
}

/*
 * Remember that we want to create a parent pointer in the tempfile.  These
 * stashed actions will be replayed later.
 */
STATIC int
xrep_pptr_add_pointer(
	struct xrep_pptrs	*rp,
	const struct xfs_name	*name,
	const struct xfs_inode	*dp,
	xfs_dir2_dataptr_t	diroffset)
{
	struct xrep_pptr	pptr = {
		.action		= XREP_PPTR_ADD,
		.namelen	= name->len,
		.p_ino		= dp->i_ino,
		.p_gen		= VFS_IC(dp)->i_generation,
		.p_diroffset	= diroffset,
	};
	int			error;

	trace_xrep_pptr_add_pointer(rp->sc->tempip, dp, diroffset, name);

	error = xfblob_store(rp->pptr_names, &pptr.name_cookie, name->name,
			name->len);
	if (error)
		return error;

	return xfarray_append(rp->parent_ptrs, &pptr);
}

/*
 * Examine an entry of a directory.  If this dirent leads us back to the file
 * whose parent pointers we're rebuilding, add a pptr to the temporary
 * directory.
 */
STATIC int
xrep_pptr_scan_dirent(
	struct xfs_scrub	*sc,
	struct xfs_inode	*dp,
	xfs_dir2_dataptr_t	dapos,
	const struct xfs_name	*name,
	xfs_ino_t		ino,
	void			*priv)
{
	struct xrep_pptrs	*rp = priv;
	int			error;

	/* Dirent doesn't point to this directory. */
	if (ino != rp->sc->ip->i_ino)
		return 0;

	/* No weird looking names. */
	if (!xfs_dir2_namecheck(name->name, name->len))
		return -EFSCORRUPTED;

	/* No mismatching ftypes. */
	if (name->type != xfs_mode_to_ftype(VFS_I(sc->ip)->i_mode))
		return -EFSCORRUPTED;

	/* Don't pick up dot or dotdot entries; we only want child dirents. */
	if (xrep_pptr_samename(name, &xfs_name_dotdot) ||
	    xrep_pptr_samename(name, &xfs_name_dot))
		return 0;

	/*
	 * Transform this dirent into a parent pointer and queue it for later
	 * addition to the temporary file.
	 */
	mutex_lock(&rp->lock);
	error = xrep_pptr_add_pointer(rp, name, dp, dapos);
	mutex_unlock(&rp->lock);
	return error;
}

/*
 * Decide if we want to look for dirents in this directory.  Skip the file
 * being repaired and any files being used to stage repairs.
 */
static inline bool
xrep_pptr_want_scan(
	struct xrep_pptrs	*rp,
	const struct xfs_inode	*ip)
{
	return ip != rp->sc->ip && !xrep_is_tempfile(ip);
}

/*
 * Take ILOCK on a file that we want to scan.
 *
 * Select ILOCK_EXCL if the file is a directory with an unloaded data bmbt.
 * Otherwise, take ILOCK_SHARED.
 */
static inline unsigned int
xrep_pptr_scan_ilock(
	struct xrep_pptrs	*rp,
	struct xfs_inode	*ip)
{
	uint			lock_mode = XFS_ILOCK_SHARED;

	/* Still need to take the shared ILOCK to advance the iscan cursor. */
	if (!xrep_pptr_want_scan(rp, ip))
		goto lock;

	if (S_ISDIR(VFS_I(ip)->i_mode) && xfs_need_iread_extents(&ip->i_df)) {
		lock_mode = XFS_ILOCK_EXCL;
		goto lock;
	}

lock:
	xfs_ilock(ip, lock_mode);
	return lock_mode;
}

/*
 * Scan this file for relevant child dirents that point to the file whose
 * parent pointers we're rebuilding.
 */
STATIC int
xrep_pptr_scan_file(
	struct xrep_pptrs	*rp,
	struct xfs_inode	*ip)
{
	unsigned int		lock_mode;
	int			error = 0;

	lock_mode = xrep_pptr_scan_ilock(rp, ip);

	if (!xrep_pptr_want_scan(rp, ip))
		goto scan_done;

	if (S_ISDIR(VFS_I(ip)->i_mode)) {
		error = xchk_dir_walk(rp->sc, ip, xrep_pptr_scan_dirent, rp);
		if (error)
			goto scan_done;
	}

scan_done:
	xchk_iscan_mark_visited(&rp->iscan, ip);
	xfs_iunlock(ip, lock_mode);
	return error;
}

/* Scan all files in the filesystem for parent pointers. */
STATIC int
xrep_pptr_scan_dirtree(
	struct xrep_pptrs	*rp)
{
	struct xfs_scrub	*sc = rp->sc;
	struct xfs_inode	*ip;
	int			error;

	/*
	 * Filesystem scans are time consuming.  Drop the file ILOCK and all
	 * other resources for the duration of the scan and hope for the best.
	 */
	xchk_trans_cancel(sc);
	if (sc->ilock_flags & (XFS_ILOCK_SHARED | XFS_ILOCK_EXCL))
		xchk_iunlock(sc, sc->ilock_flags & (XFS_ILOCK_SHARED |
						    XFS_ILOCK_EXCL));
	error = xchk_trans_alloc_empty(sc);
	if (error)
		return error;

	while ((error = xchk_iscan_iter(sc, &rp->iscan, &ip)) == 1) {
		uint64_t	mem_usage;

		error = xrep_pptr_scan_file(rp, ip);
		xchk_irele(sc, ip);
		if (error)
			break;

		/* Flush stashed pptr updates to constrain memory usage. */
		mutex_lock(&rp->lock);
		mem_usage = xfarray_bytes(rp->parent_ptrs) +
			     xfblob_bytes(rp->pptr_names);
		mutex_unlock(&rp->lock);
		if (mem_usage >= MAX_PPTR_STASH_SIZE) {
			xchk_trans_cancel(sc);

			error = xrep_tempfile_iolock_polled(sc);
			if (error)
				break;

			error = xrep_pptr_replay_updates(rp);
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

/* Dump a parent pointer from the temporary file. */
STATIC int
xrep_pptr_dump_tempptr(
	struct xfs_scrub	*sc,
	struct xfs_inode	*ip,
	unsigned int		attr_flags,
	const unsigned char	*name,
	unsigned int		namelen,
	const void		*value,
	unsigned int		valuelen,
	void			*priv)
{
	struct xrep_pptrs	*rp = priv;
	const struct xfs_parent_name_rec *rec = (const void *)name;

	if (!(attr_flags & XFS_ATTR_PARENT))
		return 0;

	if (!xfs_parent_namecheck(sc->mp, rec, namelen, attr_flags) ||
	    !xfs_parent_valuecheck(sc->mp, value, valuelen))
		return -EFSCORRUPTED;

	xfs_parent_irec_from_disk(&rp->pptr, rec, value, valuelen);

	trace_xrep_pptr_dumpname(sc->tempip, &rp->pptr);
	return 0;
}

/*
 * "Commit" the new parent pointer (aka extended attribute) structure to the
 * file that we're repairing.
 *
 * In the final version, we'd copy the existing xattrs from the file being
 * repaired to the temporary file and swap the new xattr contents (which we
 * created in the tempfile) into the file being repaired.  For now we just lock
 * the temporary file and dump what we found.
 */
STATIC int
xrep_pptr_rebuild_tree(
	struct xrep_pptrs	*rp)
{
	struct xfs_scrub	*sc = rp->sc;
	int			error = 0;

	/*
	 * Replay the last of the stashed dirent updates after retaking
	 * IOLOCK_EXCL of the directory that we're repairing and the temporary
	 * directory.
	 */
	xchk_trans_cancel(sc);

	ASSERT(sc->ilock_flags & XFS_IOLOCK_EXCL);
	error = xrep_tempfile_iolock_polled(sc);
	if (error)
		return error;

	error = xrep_pptr_replay_updates(rp);
	if (error)
		return error;

	/*
	 * At this point, we've quiesced both files and should be ready
	 * to commit the new contents.
	 *
	 * We don't have atomic swapext here, so all we do is dump the pptrs
	 * that we found to the ftrace buffer.  Inactivation of the tempfile
	 * will erase the attr fork for us.
	 */
	error = xchk_trans_alloc(sc, 0);
	if (error)
		return error;

	trace_xrep_pptr_rebuild_tree(sc->ip, 0);

	xrep_tempfile_ilock(sc);
	return xchk_xattr_walk(sc, sc->tempip, xrep_pptr_dump_tempptr, rp);
}

/* Set up the filesystem scan so we can look for pptrs. */
STATIC int
xrep_pptr_setup_scan(
	struct xrep_pptrs	*rp)
{
	struct xfs_scrub	*sc = rp->sc;
	int			error;

	/* Set up some staging memory for logging parent pointers. */
	error = xfarray_create(sc->mp, "parent pointers", 0,
			sizeof(struct xrep_pptr), &rp->parent_ptrs);
	if (error)
		return error;

	error = xfblob_create(sc->mp, "pptr names", &rp->pptr_names);
	if (error)
		goto out_entries;

	mutex_init(&rp->lock);

	/* Retry iget every tenth of a second for up to 30 seconds. */
	xchk_iscan_start(&rp->iscan, 30000, 100);

	return 0;

out_entries:
	xfarray_destroy(rp->parent_ptrs);
	return error;
}

/* Repair the parent pointers. */
int
xrep_parent(
	struct xfs_scrub	*sc)
{
	struct xrep_pptrs	*rp = sc->buf;
	int			error = 0;

	/* We require directory parent pointers to rebuild anything. */
	if (!xfs_has_parent(sc->mp))
		return -EOPNOTSUPP;

	error = xrep_pptr_setup_scan(rp);
	if (error)
		goto out;

	error = xrep_pptr_scan_dirtree(rp);
	if (error)
		goto out_finish_scan;

	error = xrep_pptr_rebuild_tree(rp);
	if (error)
		goto out_finish_scan;

out_finish_scan:
	xrep_pptr_teardown(rp);
out:
	return error;
}
