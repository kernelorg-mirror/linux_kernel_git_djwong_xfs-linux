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
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "xfs_attr.h"
#include "xfs_parent.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/bitmap.h"
#include "scrub/xfile.h"
#include "scrub/xfarray.h"
#include "scrub/xfblob.h"
#include "scrub/listxattr.h"
#include "scrub/trace.h"
#include "scrub/dirloop.h"

/* Clean up the dirloop checking resources. */
STATIC void
xchk_dirloop_buf_cleanup(
	void				*buf)
{
	struct xchk_dirloop		*dl = buf;
	struct xchk_dirloop_path	*path, *n;

	xchk_dirloop_for_each_path_safe(dl, path, n) {
		list_del_init(&path->list);
		xino_bitmap_destroy(&path->seen_inodes);
		kfree(path);
	}

	if (dl->path_components)
		xfarray_destroy(dl->path_components);
	if (dl->path_names)
		xfblob_destroy(dl->path_names);
}

STATIC xfs_ino_t
xchk_ino_for_scrubbing(
	struct xfs_scrub	*sc)
{
	struct xfs_inode	*ip_in;

	if (sc->sm->sm_ino)
		return sc->sm->sm_ino;

	ip_in = XFS_I(file_inode(sc->file));
	return ip_in->i_ino;
}

/* Set us up to look for directory loops. */
int
xchk_setup_dirloop(
	struct xfs_scrub		*sc)
{
	struct xchk_dirloop		*dl;
	char				*descr;
	int				error;

	dl = kvzalloc(sizeof(struct xchk_dirloop), XCHK_GFP_FLAGS);
	if (!dl)
		return -ENOMEM;
	dl->sc = sc;
	INIT_LIST_HEAD(&dl->path_list);
	sc->buf = dl;
	sc->buf_cleanup = xchk_dirloop_buf_cleanup;

	descr = kasprintf(XCHK_GFP_FLAGS,
			"XFS (%s): inode 0x%llx dirloop path names",
			sc->mp->m_super->s_id, xchk_ino_for_scrubbing(sc));
	error = xfblob_create(descr, &dl->path_names);
	kfree(descr);
	if (error)
		return error;

	descr = kasprintf(XCHK_GFP_FLAGS,
			"XFS (%s): inode 0x%llx dirloop path components",
			sc->mp->m_super->s_id, xchk_ino_for_scrubbing(sc));
	error = xfarray_create(descr, 0, sizeof(struct xchk_dirloop_path_part),
			&dl->path_components);
	kfree(descr);
	if (error)
		return error;

	return xchk_setup_inode_contents(sc, 0);
}

/*
 * Add the parent pointer described by @dl->pptr to the given path as a new
 * component.
 */
STATIC int
xchk_dirloop_path_append(
	struct xchk_dirloop		*dl,
	struct xfs_inode		*ip,
	struct xchk_dirloop_path	*path)
{
	struct xchk_dirloop_path_part	component = {
		.parent_ino		= dl->pptr.p_ino,
		.parent_gen		= dl->pptr.p_gen,
		.name_len		= dl->pptr.p_namelen,
	};
	int				error;

	error = xfblob_store(dl->path_names, &component.name_cookie,
			dl->pptr.p_name, dl->pptr.p_namelen);
	if (error)
		return error;

	error = xino_bitmap_set(&path->seen_inodes, ip->i_ino);
	if (error)
		return error;

	error = xfarray_append(dl->path_components, &component);
	if (error)
		return error;

	path->nr_components++;
	return 0;
}

/*
 * Create an xchk_path for a parent pointer of the directory that we're
 * scanning.  For each path encountered, we try to walk towards the root with
 * the goal of deleting all parents except for one that leads to the root.
 *
 * We use -EFSCORRUPTED to signal that the inode being scanned has a corrupt
 * parent pointer and hence there's no point in continuing.
 */
STATIC int
xchk_dirloop_walk_pstart_parents(
	struct xfs_scrub		*sc,
	struct xfs_inode		*ip,
	unsigned int			attr_flags,
	const unsigned char		*name,
	unsigned int			namelen,
	const void			*value,
	unsigned int			valuelen,
	void				*priv)
{
	const struct xfs_parent_name_rec *rec = (const void *)name;
	struct xchk_dirloop		*dl = sc->buf;
	struct xchk_dirloop_path	*path;
	int				error;

	/* Ignore incomplete xattrs */
	if (attr_flags & XFS_ATTR_INCOMPLETE)
		return 0;

	/* Ignore anything that isn't a parent pointer. */
	if (!(attr_flags & XFS_ATTR_PARENT))
		return 0;

	/* Does the ondisk parent pointer structure make sense? */
	if (!xfs_parent_namecheck(sc->mp, rec, namelen, attr_flags) ||
	    !xfs_parent_valuecheck(sc->mp, value, valuelen))
		return -EFSCORRUPTED;

	xfs_parent_irec_from_disk(&dl->pptr, rec, value, valuelen);

	trace_xchk_dirloop_walk_pstart_parents(sc->ip, ip, dl->nr_paths,
			&dl->pptr);

	/*
	 * Create a new xchk_path structure to remember this parent pointer
	 * and record the first name component.
	 */
	path = kmalloc(sizeof(struct xchk_dirloop_path), XCHK_GFP_FLAGS);
	if (!path)
		return -ENOMEM;
	INIT_LIST_HEAD(&path->list);
	xino_bitmap_init(&path->seen_inodes);
	path->nr_components = 0;
	path->outcome = PATH_SCANNING;

	error = xchk_dirloop_path_append(dl, sc->ip, path);
	if (error)
		goto out_path;

	path->first_component = xfarray_length(dl->path_components) - 1;
	path->second_component = XFARRAY_NULLIDX;

	list_add_tail(&dl->path_list, &path->list);
	dl->nr_paths++;
	return 0;
out_path:
	kfree(path);
	return error;
}

/*
 * (Re)load the first parent pointer of this path into the parent pointer
 * scratch pad in preparation to walk up the directory tree towards the root,
 * and make sure it's still there.  Returns -ECANCELED to skip processing the
 * rest of the path data.
 */
STATIC int
xchk_dirloop_revalidate_pstart(
	struct xchk_dirloop		*dl,
	struct xchk_dirloop_path	*path)
{
	struct xchk_dirloop_path_part	component;
	struct xfs_scrub		*sc = dl->sc;
	int				error;

	error = xfarray_load(dl->path_components, path->first_component,
			&component);
	if (error)
		return error;

	/*
	 * Check that this parent pointer is still attached to the inode that
	 * we're scanning/
	 */
	dl->pptr.p_ino = component.parent_ino;
	dl->pptr.p_gen = component.parent_gen;
	dl->pptr.p_namelen = component.name_len;

	error = xfblob_load(dl->path_names, component.name_cookie,
			dl->pptr.p_name, component.name_len);
	if (error)
		return error;

	xfs_parent_irec_hashname(sc->mp, &dl->pptr);
	error = xfs_parent_lookup(sc->tp, sc->ip, &dl->pptr, &dl->scratch);
	if (error == -ENOATTR) {
		/*
		 * The parent pointer has disappeared on us.  Dump all the scan
		 * results and try again.
		 */
		dl->invalid = true;
		return -ECANCELED;
	}

	return error;
}

/*
 * Walk the parent pointers of a directory at the end of a path and record
 * the parent that we find.
 */
STATIC int
xchk_dirloop_walk_pend_parents(
	struct xfs_scrub		*sc,
	struct xfs_inode		*ip,
	unsigned int			attr_flags,
	const unsigned char		*name,
	unsigned int			namelen,
	const void			*value,
	unsigned int			valuelen,
	void				*priv)
{
	const struct xfs_parent_name_rec *rec = (const void *)name;
	struct xchk_dirloop		*dl = sc->buf;
	struct xchk_dirloop_path	*path = priv;

	/* Ignore incomplete xattrs */
	if (attr_flags & XFS_ATTR_INCOMPLETE)
		return 0;

	/* Ignore anything that isn't a parent pointer. */
	if (!(attr_flags & XFS_ATTR_PARENT))
		return 0;

	/* Does the ondisk parent pointer structure make sense? */
	if (!xfs_parent_namecheck(sc->mp, rec, namelen, attr_flags) ||
	    !xfs_parent_valuecheck(sc->mp, value, valuelen))
		return -EFSCORRUPTED;

	/*
	 * If we've already set @dl->pptr.p_ino, then this directory has
	 * multiple parents.  Signal this back to the caller via -EMLINK.
	 */
	if (dl->pptr.p_ino != NULLFSINO)
		return -EMLINK;

	xfs_parent_irec_from_disk(&dl->pptr, rec, value, valuelen);

	trace_xchk_dirloop_walk_pend_parents(sc->ip, ip, path->nr_components,
			&dl->pptr);

	return 0;
}

/*
 * Scan the directory at the end of this path for its parent directory.  If we
 * find one, extend the path.  Returns -ECANCELED to skip processing the rest
 * of the path data.
 */
STATIC int
xchk_dirloop_grow_path_once(
	struct xchk_dirloop		*dl,
	struct xchk_dirloop_path	*path,
	bool				is_metadir)
{
	struct xfs_scrub		*sc = dl->sc;
	struct xfs_inode		*dp;
	unsigned int			lock_mode;
	int				error;

	/* Grab and lock the parent directory. */
	error = xchk_iget(sc, dl->pptr.p_ino, &dp);
	if (error)
		return error;

	lock_mode = xfs_ilock_attr_map_shared(dp);

	/* We've reached the root directory; the path is ok. */
	if (dl->pptr.p_ino == dl->rootip->i_ino) {
		path->outcome = PATH_OK;
		error = 0;
		goto out_iunlock;
	}

	/*
	 * The inode being scanned is its own distant ancestor!  Get rid of
	 * this path.
	 */
	if (dl->pptr.p_ino == sc->ip->i_ino) {
		path->outcome = PATH_DELETE;
		error = 0;
		goto out_iunlock;
	}

	/*
	 * We've seen this inode before during the path walk.  There's a loop
	 * above us in the directory tree.  This probably means that we cannot
	 * continue, but let's keep walking paths to get a full picture.
	 */
	if (xino_bitmap_test(&path->seen_inodes, dl->pptr.p_ino)) {
		path->outcome = PATH_LOOP;
		error = 0;
		goto out_iunlock;
	}

	/*
	 * Generation number didn't match, which means this the parent pointer
	 * is stale.  Start over.
	 */
	if (VFS_I(dp)->i_generation != dl->pptr.p_gen) {
		dl->invalid = true;
		path->outcome = PATH_STALE;
		error = -ECANCELED;
		goto out_iunlock;
	}

	/*
	 * The parent pointer handle matches, but something odd happened:
	 *
	 * 1. A non-directory is the parent of a directory in this path;
	 * 2. The parent pointer handle matches, but the path descends from an
	 *    unlinked directory, which the VFS (theoretically) prohibits.
	 * 3. The path crosses between the regular and metadata directory tree.
	 *
	 * These situations do not make sense.  If @dp is the direct parent of
	 * the directory being scanned, delete this path.  Otherwise, there is
	 * corruption further up in the directory tree, so mark the path.
	 * Either way, keep looking for more paths.
	 */
	if (!S_ISDIR(VFS_I(dp)->i_mode) ||
	    VFS_I(dp)->i_nlink == 0 ||
	    is_metadir != xfs_is_metadir_inode(dp)) {
		if (path->nr_components == 1)
			path->outcome = PATH_DELETE;
		else
			path->outcome = PATH_CORRUPT;
		error = 0;
		goto out_iunlock;
	}

	/*
	 * Walk the parent pointers of @dp to find the parent of this
	 * directory.
	 */
	dl->pptr.p_ino = NULLFSINO;
	error = xchk_xattr_walk(sc, dp, xchk_dirloop_walk_pend_parents, NULL,
			path);
	if (error == -EFSCORRUPTED || error == -EMLINK ||
	    (!error && dl->pptr.p_ino == NULLFSINO)) {
		/*
		 * Further up the directory tree from @sc->ip, we found a
		 * corrupt parent pointer, multiple parent pointers while
		 * finding this directory's parent, or zero parents despite
		 * having a nonzero link count.  Keep looking for other paths.
		 */
		path->outcome = PATH_CORRUPT;
		error = 0;
		goto out_iunlock;
	}
	if (error)
		goto out_iunlock;

	/* Append to the path components */
	error = xchk_dirloop_path_append(dl, dp, path);
	if (error)
		goto out_iunlock;

	if (path->second_component == XFARRAY_NULLIDX)
		path->second_component =
				xfarray_length(dl->path_components) - 1;

out_iunlock:
	xfs_iunlock(dp, lock_mode);
	xchk_irele(sc, dp);
	return error;
}

/*
 * Walk the directory tree upwards towards what is hopefully the root
 * directory, recording path components as we go.  Returns -ECANCELED to
 * skip processing the rest of the path data.
 */
STATIC int
xchk_dirloop_grow_path(
	struct xchk_dirloop		*dl,
	struct xchk_dirloop_path	*path)
{
	struct xfs_scrub		*sc = dl->sc;
	bool				is_metadir;
	int				error;

	ASSERT(sc->ilock_flags & XFS_ILOCK_EXCL);

	/* Reload the start of this path. */
	error = xchk_dirloop_revalidate_pstart(dl, path);
	if (error)
		return error;

	trace_xchk_dirloop_grow_path(sc->ip, sc->ip, 0, &dl->pptr);

	/*
	 * The inode being scanned is its own direct ancestor!
	 * Get rid of this path.
	 */
	if (dl->pptr.p_ino == sc->ip->i_ino) {
		path->outcome = PATH_DELETE;
		return 0;
	}

	/*
	 * Drop ILOCK_EXCL on the inode being scanned.  We still hold
	 * IOLOCK_EXCL on it, so it cannot move around or be renamed.
	 *
	 * Beyond this point we're walking up the directory tree, which means
	 * that we can acquire and drop the ILOCK on an alias of sc->ip.  The
	 * ILOCK state is no longer tracked in the scrub context.  Hence we
	 * must drop @sc->ip's ILOCK during the walk.
	 */
	is_metadir = xfs_is_metadir_inode(sc->ip);
	xchk_iunlock(sc, XFS_ILOCK_EXCL);

	/* Grow this path towards the root one directory at a time. */
	do {
		error = xchk_dirloop_grow_path_once(dl, path, is_metadir);
		if (error)
			break;
	} while (path->outcome == PATH_SCANNING);

	xchk_ilock(sc, XFS_ILOCK_EXCL);
	return error;
}

/* Dump a path part. */
STATIC void
xchk_dirloop_dump_path_part(
	struct xchk_dirloop		*dl,
	struct xchk_dirloop_path	*path,
	unsigned long long		path_nr,
	xfarray_idx_t			idx,
	unsigned long long		component_nr)
{
	struct xchk_dirloop_path_part	component;
	struct xfs_scrub		*sc = dl->sc;
	int				error;

	error = xfarray_load(dl->path_components, idx, &component);
	if (error)
		return;

	error = xfblob_load(dl->path_names, component.name_cookie,
			dl->pptr.p_name, component.name_len);
	if (error)
		return;

	dl->pptr.p_ino = component.parent_ino;
	dl->pptr.p_gen = component.parent_gen;
	dl->pptr.p_namelen = component.name_len;

	trace_xchk_dirloop_dump_path_part(sc->ip, path_nr, component_nr,
			&dl->pptr);
}

/* Dump a path. */
STATIC void
xchk_dirloop_dump_path(
	struct xchk_dirloop		*dl,
	struct xchk_dirloop_path	*path,
	unsigned long long		path_nr)
{
	struct xfs_scrub		*sc = dl->sc;
	xfarray_idx_t			idx;
	unsigned long long		i;

	trace_xchk_dirloop_dump(sc->ip, dl->rootip->i_ino, dl->nr_paths);
	trace_xchk_dirloop_dump_path(sc->ip, path_nr, path->nr_components,
			path->outcome);

	if (path->nr_components > 0)
		xchk_dirloop_dump_path_part(dl, path, path_nr,
				path->first_component, 0);

	for (i = 1, idx = path->second_component;
	     i < path->nr_components;
	     i++, idx++)
		xchk_dirloop_dump_path_part(dl, path, path_nr, idx, i);
}

/* Delete all the collected path information. */
STATIC void
xchk_dirloop_reset(
	void				*buf)
{
	struct xchk_dirloop		*dl = buf;
	struct xchk_dirloop_path	*path, *n;

	ASSERT(dl->sc->ilock_flags & XFS_ILOCK_EXCL);

	xchk_dirloop_for_each_path_safe(dl, path, n) {
		list_del_init(&path->list);
		xino_bitmap_destroy(&path->seen_inodes);
		kfree(path);
	}
	dl->nr_paths = 0;

	xfarray_truncate(dl->path_components);
	xfblob_truncate(dl->path_names);

	dl->invalid = false;
}

/*
 * For each parent pointer of this subdir, trace a path upwards towards the
 * root directory and record what we find.  Returns 0 for success,
 * -EFSCORRUPTED if walking the parent pointers of @sc->ip failed, or a
 *  negative errno.
 */
STATIC int
xchk_dirloop_find_paths_to_root(
	struct xchk_dirloop		*dl)
{
	struct xfs_scrub		*sc = dl->sc;
	struct xchk_dirloop_path	*path;
	int				error = 0;
	
	do {
		if (xchk_should_terminate(sc, &error))
			return error;

		xchk_dirloop_reset(dl);

		/*
		 * Create path walk contexts for each parent of the directory
		 * that is being scanned.  Directories are supposed to have
		 * only one parent, but this is how we detect multiple parents.
		 */
		error = xchk_xattr_walk(sc, sc->ip,
				xchk_dirloop_walk_pstart_parents, NULL, dl);
		if (error)
			return error;

		xchk_dirloop_for_each_path(dl, path) {
			/*
			 * Try to walk up each path to the root.  This enables
			 * us to find directory loops in ancestors, and the
			 * like.
			 */
			error = xchk_dirloop_grow_path(dl, path);
			if (error == -ECANCELED) {
				/* This had better be an invalidation. */
				ASSERT(dl->invalid);
				error = 0;
				break;
			}
			if (error == -EFSCORRUPTED) {
				/* Mark this path corrupt but keep searching. */
				path->outcome = PATH_CORRUPT;
				error = 0;
			}
			if (error)
				return error;
		}
	} while (dl->invalid);

	return 0;
}

/* Figure out what to do with the paths we tried to find. */
STATIC int
xchk_dirloop_evaluate_paths(
	struct xchk_dirloop		*dl,
	struct xchk_dirloop_outcomes	*oc)
{
	struct xchk_dirloop_path	*path;

	memset(oc, 0, sizeof(struct xchk_dirloop_outcomes));

	/* Scan data invalid; restart the whole thing. */
	if (dl->invalid)
		return -EDEADLOCK;

	/* Too many parent links; abort to avoid integer overflows. */
	if (dl->nr_paths > XFS_MAXLINK)
		return -EFSCORRUPTED;

	/* Scan the paths we have to decide what to do. */
	xchk_dirloop_for_each_path(dl, path) {
		switch (path->outcome) {
		case PATH_SCANNING:
			/* shouldn't get here */
			ASSERT(0);
			return -EIO;
		case PATH_DELETE:
			/* This one is already going away. */
			oc->bad++;
			break;
		case PATH_CORRUPT:
		case PATH_LOOP:
			/* Couldn't find the end of this path. */
			oc->suspect++;
			break;
		case PATH_STALE:
			/* Rescan the whole thing. */
			return -EDEADLOCK;
		case PATH_OK:
			/* This path got all the way to the root. */
			oc->good++;
			break;
		}
	}

	return 0;
}

/* Look for directory loops. */
int
xchk_dirloop(
	struct xfs_scrub		*sc)
{
	struct xchk_dirloop_outcomes	oc;
	struct xchk_dirloop		*dl = sc->buf;
	struct xchk_dirloop_path	*path;
	unsigned long long		nr;
	int				error;

	/*
	 * Nondirectories do not point downwards to other files, so they cannot
	 * cause a cycle in the directory tree.  Unlinked directories cannot be
	 * part of a cycle, so we skip them.
	 */
	if (!S_ISDIR(VFS_I(sc->ip)->i_mode) || VFS_I(sc->ip)->i_nlink == 0)
		return -ENOENT;

	ASSERT(xfs_has_parent(sc->mp));

	/* Find the root of the directory tree. */
	if (xfs_is_metadir_inode(sc->ip))
		dl->rootip = sc->mp->m_metadirip;
	else
		dl->rootip = sc->mp->m_rootip;
	if (sc->ip == dl->rootip)
		return 0;

	/* Trace each parent pointer's path to the root. */
	error = xchk_dirloop_find_paths_to_root(dl);
	if (error == -EFSCORRUPTED) {
		/*
		 * Don't bother walking the paths if the xattr structure or the
		 * parent pointers are corrupt; this scan cannot be completed
		 * without that information.
		 */
		xchk_ino_xref_set_corrupt(sc, sc->ip->i_ino);
		xchk_set_incomplete(sc);
		return 0;
	}
	if (error)
		return error;

	/* Assess what we found in our path evaluation. */
	error = xchk_dirloop_evaluate_paths(dl, &oc);
	if (error == -EFSCORRUPTED) {
		/* Found too many paths to cross-reference. */
		xchk_ino_xref_set_corrupt(sc, sc->ip->i_ino);
		return 0;
	}
	if (error)
		return error;

	if (oc.bad || oc.good + oc.suspect != 1)
		xchk_ino_set_corrupt(sc, sc->ip->i_ino);
	if (oc.suspect)
		xchk_ino_xref_set_corrupt(sc, sc->ip->i_ino);

	/* Dump the paths we found. */
	nr = 0;
	xchk_dirloop_for_each_path(dl, path)
		xchk_dirloop_dump_path(dl, path, nr++);

	return 0;
}
