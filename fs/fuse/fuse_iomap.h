// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef _FS_FUSE_IOMAP_H
#define _FS_FUSE_IOMAP_H

#if IS_ENABLED(CONFIG_FUSE_IOMAP)
enum fuse_iomap_iodir {
	READ_MAPPING,
	WRITE_MAPPING,
};

bool fuse_iomap_enabled(void);

static inline bool fuse_has_iomap(const struct inode *inode)
{
	return get_fuse_conn(inode)->iomap;
}

extern const struct fuse_backing_ops fuse_iomap_backing_ops;

void fuse_iomap_mount(struct fuse_mount *fm);
void fuse_iomap_unmount(struct fuse_mount *fm);

void fuse_iomap_init_inode(struct inode *inode, struct fuse_attr *attr);
void fuse_iomap_evict_inode(struct inode *inode);

static inline bool fuse_inode_has_iomap(const struct inode *inode)
{
	const struct fuse_inode *fi = get_fuse_inode(inode);

	return test_bit(FUSE_I_IOMAP, &fi->state);
}

int fuse_iomap_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		      u64 start, u64 length);
loff_t fuse_iomap_lseek(struct file *file, loff_t offset, int whence);
sector_t fuse_iomap_bmap(struct address_space *mapping, sector_t block);
#else
# define fuse_iomap_enabled(...)		(false)
# define fuse_has_iomap(...)			(false)
# define fuse_iomap_mount(...)			((void)0)
# define fuse_iomap_unmount(...)		((void)0)
# define fuse_iomap_init_inode(...)		((void)0)
# define fuse_iomap_evict_inode(...)		((void)0)
# define fuse_inode_has_iomap(...)		(false)
# define fuse_iomap_fiemap			NULL
# define fuse_iomap_lseek(...)			(-ENOSYS)
# define fuse_iomap_bmap(...)			(-ENOSYS)
#endif /* CONFIG_FUSE_IOMAP */

#endif /* _FS_FUSE_IOMAP_H */
