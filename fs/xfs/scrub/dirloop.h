/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SCRUB_DIRLOOP_H__
#define __XFS_SCRUB_DIRLOOP_H__

/*
 * Each of these represents one parent pointer path component in a chain going
 * up towards the directory tree root.  These are stored inside an xfarray.
 */
struct xchk_dirloop_path_part {
	/* Directory entry name associated with this parent link. */
	xfblob_cookie		name_cookie;
	unsigned int		name_len;

	/* Handle of the parent directory. */
	unsigned int		parent_gen;
	xfs_ino_t		parent_ino;
};

enum xchk_dirloop_path_resolution {
	PATH_SCANNING = 0,	/* still being put together */
	PATH_DELETE,		/* delete this path */
	PATH_CORRUPT,		/* corruption detected in path */
	PATH_LOOP,		/* cycle detected further up in the dir tree */
	PATH_STALE,		/* path is stale */
	PATH_OK,		/* path reaches the root */
};

/*
 * Each of these represents one parent pointer path out of the directory being
 * scanned.  These exist in-core, and hopefully there aren't more than a
 * handful of them.
 */
struct xchk_dirloop_path {
	struct list_head	list;

	/* Index of the first component in this path. */
	xfarray_idx_t		first_component;

	/* Index of the second component in this path. */
	xfarray_idx_t		second_component;

	/* Inodes seen while walking this path. */
	struct xino_bitmap	seen_inodes;

	/* Number of components in this path. */
	unsigned long long	nr_components;

	/* What did we conclude from following this path? */
	enum xchk_dirloop_path_resolution outcome;
};

struct xchk_dirloop_outcomes {
	/* Number of PATH_DELETE */
	unsigned int			bad;

	/* Number of PATH_CORRUPT or PATH_LOOP */
	unsigned int			suspect;

	/* Number of PATH_OK */
	unsigned int			good;
};

struct xchk_dirloop {
	struct xfs_scrub	*sc;

	/* Root inode that we're looking for. */
	struct xfs_inode	*rootip;

	/*
	 * Handle for the inode that we're scanning.  The hook is not removed
	 * until after @sc->ip is released, so save an extra copy here.
	 */
	xfs_ino_t		ino;
	unsigned int		gen;

	/* Scratch buffer for scanning pptr xattrs */
	struct xfs_parent_scratch scratch;
	struct xfs_parent_name_irec pptr;

	/*
	 * Hook into directory updates so that we can receive live updates
	 * from other writer threads.
	 */
	struct xfs_dir_hook	hooks;

	/* lock for everything below here */
	struct mutex		lock;

	/* buffer for the live update functions to use for dirent names */
	unsigned char		hook_namebuf[MAXNAMELEN];

	/*
	 * All path components observed during this scan.  Each of the path
	 * components for a particular pathwalk are recorded in sequential
	 * order in the xfarray.  A pathwalk ends either with a component
	 * pointing to the root directory (success) or pointing to NULLFSINO
	 * (loop detected, empty dir detected, etc).
	 */
	struct xfarray		*path_components;

	/* All names observed during this scan. */
	struct xfblob		*path_names;

	/* All paths being tracked by this scanner. */
	struct list_head	path_list;

	/* Number of paths in path_list. */
	unsigned long long	nr_paths;

	/* Have the path data been invalidated by a concurrent update? */
	bool			invalid:1;

	/* Has the scan been aborted? */
	bool			aborted:1;
};

#define xchk_dirloop_for_each_path_safe(dl, path, n) \
	list_for_each_entry_safe((path), (n), &(dl)->path_list, list)

#define xchk_dirloop_for_each_path(dl, path) \
	list_for_each_entry((path), &(dl)->path_list, list)

#endif /* __XFS_SCRUB_DIRLOOP_H__ */
