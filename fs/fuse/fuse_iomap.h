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

int fuse_iomap_mount(struct fuse_mount *fm);
void fuse_iomap_mount_async(struct fuse_mount *fm);
void fuse_iomap_unmount(struct fuse_mount *fm);

void fuse_iomap_init_inode(struct inode *inode, struct fuse_attr *attr);
void fuse_iomap_evict_inode(struct inode *inode);

static inline bool fuse_inode_has_iomap(const struct inode *inode)
{
	const struct fuse_inode *fi = get_fuse_inode(inode);

	return test_bit(FUSE_I_IOMAP, &fi->state);
}

static inline bool fuse_inode_has_atomic(const struct inode *inode)
{
	const struct fuse_inode *fi = get_fuse_inode(inode);

	return test_bit(FUSE_I_ATOMIC, &fi->state);
}

int fuse_iomap_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		      u64 start, u64 length);
loff_t fuse_iomap_lseek(struct file *file, loff_t offset, int whence);
sector_t fuse_iomap_bmap(struct address_space *mapping, sector_t block);

void fuse_iomap_open(struct inode *inode, struct file *file);
int fuse_iomap_finish_open(const struct fuse_file *ff,
			   const struct inode *inode);
void fuse_iomap_open_truncate(struct inode *inode);

void fuse_iomap_set_disk_size(struct fuse_inode *fi, loff_t newsize);
static inline loff_t fuse_iomap_get_disk_size(const struct fuse_inode *fi)
{
	/* unlocked access, for tracing only */
	return fuse_inode_has_iomap(&fi->inode) ? fi->i_disk_size : 0;
}
int fuse_iomap_setsize_finish(struct inode *inode, loff_t newsize);

ssize_t fuse_iomap_read_iter(struct kiocb *iocb, struct iov_iter *to);
ssize_t fuse_iomap_write_iter(struct kiocb *iocb, struct iov_iter *from);

int fuse_iomap_mmap(struct file *file, struct vm_area_struct *vma);
int fuse_iomap_setsize_start(struct inode *inode, loff_t newsize);
int fuse_iomap_fallocate(struct file *file, int mode, loff_t offset,
			 loff_t length, loff_t new_size);
int fuse_iomap_flush_unmap_range(struct inode *inode, loff_t pos,
				 loff_t endpos);
void fuse_iomap_copied_file_range(struct inode *inode, loff_t offset,
				  u64 written);

int fuse_dev_ioctl_iomap_support(struct file *file,
				 struct fuse_iomap_support __user *argp);
int fuse_iomap_dev_inval(struct fuse_conn *fc,
			 const struct fuse_iomap_dev_inval_out *arg);

int fuse_iomap_fadvise(struct file *file, loff_t start, loff_t end, int advice);
int fuse_dev_ioctl_iomap_set_nofs(struct file *file, uint32_t __user *argp);
#else
# define fuse_iomap_enabled(...)		(false)
# define fuse_has_iomap(...)			(false)
# define fuse_iomap_mount(...)			(0)
# define fuse_iomap_mount_async(...)		((void)0)
# define fuse_iomap_unmount(...)		((void)0)
# define fuse_iomap_init_inode(...)		((void)0)
# define fuse_iomap_evict_inode(...)		((void)0)
# define fuse_inode_has_iomap(...)		(false)
# define fuse_iomap_fiemap			NULL
# define fuse_iomap_lseek(...)			(-ENOSYS)
# define fuse_iomap_bmap(...)			(-ENOSYS)
# define fuse_iomap_open(...)			((void)0)
# define fuse_iomap_finish_open(...)		(-ENOSYS)
# define fuse_iomap_open_truncate(...)		((void)0)
# define fuse_iomap_set_disk_size(...)		((void)0)
# define fuse_iomap_get_disk_size(...)		((loff_t)0)
# define fuse_iomap_setsize_finish(...)		(-ENOSYS)
# define fuse_iomap_read_iter(...)		(-ENOSYS)
# define fuse_iomap_write_iter(...)		(-ENOSYS)
# define fuse_iomap_mmap(...)			(-ENOSYS)
# define fuse_iomap_setsize_start(...)		(-ENOSYS)
# define fuse_iomap_fallocate(...)		(-ENOSYS)
# define fuse_iomap_flush_unmap_range(...)	(-ENOSYS)
# define fuse_iomap_copied_file_range(...)	((void)0)
# define fuse_dev_ioctl_iomap_support(...)	(-EOPNOTSUPP)
# define fuse_iomap_dev_inval(...)		(-ENOSYS)
# define fuse_iomap_fadvise			NULL
# define fuse_dev_ioctl_iomap_set_nofs(...)	(-EOPNOTSUPP)
#endif /* CONFIG_FUSE_IOMAP */

#endif /* _FS_FUSE_IOMAP_H */
