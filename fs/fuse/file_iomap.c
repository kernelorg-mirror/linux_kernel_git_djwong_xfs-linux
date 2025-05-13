// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org.
 */
#include "fuse_i.h"
#include "fuse_trace.h"
#include <linux/iomap.h>
#include <linux/pagemap.h>
#include <linux/falloc.h>

static bool __read_mostly enable_iomap =
#if IS_ENABLED(CONFIG_FUSE_IOMAP_BY_DEFAULT)
	true;
#else
	false;
#endif
module_param(enable_iomap, bool, 0644);
MODULE_PARM_DESC(enable_iomap, "Enable file I/O through iomap");

#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG)
# define ASSERT(a)	do { WARN_ON(!(a)); } while (0)
#else
# define ASSERT(a)
#endif

bool fuse_iomap_enabled(void)
{
	return enable_iomap;
}

static inline bool fuse_iomap_check_type(uint16_t type)
{
	BUILD_BUG_ON(FUSE_IOMAP_TYPE_HOLE	!= IOMAP_HOLE);
	BUILD_BUG_ON(FUSE_IOMAP_TYPE_DELALLOC	!= IOMAP_DELALLOC);
	BUILD_BUG_ON(FUSE_IOMAP_TYPE_MAPPED	!= IOMAP_MAPPED);
	BUILD_BUG_ON(FUSE_IOMAP_TYPE_UNWRITTEN	!= IOMAP_UNWRITTEN);
	BUILD_BUG_ON(FUSE_IOMAP_TYPE_INLINE	!= IOMAP_INLINE);

	switch (type) {
	case FUSE_IOMAP_TYPE_PURE_OVERWRITE:
	case FUSE_IOMAP_TYPE_HOLE:
	case FUSE_IOMAP_TYPE_DELALLOC:
	case FUSE_IOMAP_TYPE_MAPPED:
	case FUSE_IOMAP_TYPE_UNWRITTEN:
	case FUSE_IOMAP_TYPE_INLINE:
		return true;
	}

	return false;
}

#define FUSE_IOMAP_F_ALL (FUSE_IOMAP_F_NEW | \
			  FUSE_IOMAP_F_DIRTY | \
			  FUSE_IOMAP_F_SHARED | \
			  FUSE_IOMAP_F_MERGED | \
			  FUSE_IOMAP_F_XATTR | \
			  FUSE_IOMAP_F_BOUNDARY | \
			  FUSE_IOMAP_F_ANON_WRITE | \
			  FUSE_IOMAP_F_ATOMIC_BIO | \
			  FUSE_IOMAP_F_WANT_IOMAP_END)

static inline bool fuse_iomap_check_flags(uint16_t flags)
{
	BUILD_BUG_ON(FUSE_IOMAP_F_NEW		!= IOMAP_F_NEW);
	BUILD_BUG_ON(FUSE_IOMAP_F_DIRTY		!= IOMAP_F_DIRTY);
	BUILD_BUG_ON(FUSE_IOMAP_F_SHARED	!= IOMAP_F_SHARED);
	BUILD_BUG_ON(FUSE_IOMAP_F_MERGED	!= IOMAP_F_MERGED);
	BUILD_BUG_ON(FUSE_IOMAP_F_XATTR		!= IOMAP_F_XATTR);
	BUILD_BUG_ON(FUSE_IOMAP_F_BOUNDARY	!= IOMAP_F_BOUNDARY);
	BUILD_BUG_ON(FUSE_IOMAP_F_ANON_WRITE	!= IOMAP_F_ANON_WRITE);
	BUILD_BUG_ON(FUSE_IOMAP_F_ATOMIC_BIO	!= IOMAP_F_ATOMIC_BIO);
	BUILD_BUG_ON(FUSE_IOMAP_F_WANT_IOMAP_END != IOMAP_F_PRIVATE);

	return (flags & ~FUSE_IOMAP_F_ALL) == 0;
}

/* Check the incoming mappings to make sure they're not nonsense */
static inline int fuse_iomap_validate(const struct fuse_iomap_begin_out *outarg,
				      unsigned opflags, loff_t pos)
{
	BUILD_BUG_ON(FUSE_IOMAP_OP_WRITE	!= IOMAP_WRITE);
	BUILD_BUG_ON(FUSE_IOMAP_OP_ZERO		!= IOMAP_ZERO);
	BUILD_BUG_ON(FUSE_IOMAP_OP_REPORT	!= IOMAP_REPORT);
	BUILD_BUG_ON(FUSE_IOMAP_OP_FAULT	!= IOMAP_FAULT);
	BUILD_BUG_ON(FUSE_IOMAP_OP_DIRECT	!= IOMAP_DIRECT);
	BUILD_BUG_ON(FUSE_IOMAP_OP_NOWAIT	!= IOMAP_NOWAIT);
	BUILD_BUG_ON(FUSE_IOMAP_OP_OVERWRITE_ONLY != IOMAP_OVERWRITE_ONLY);
	BUILD_BUG_ON(FUSE_IOMAP_OP_UNSHARE	!= IOMAP_UNSHARE);
	BUILD_BUG_ON(FUSE_IOMAP_OP_ATOMIC	!= IOMAP_ATOMIC);
	BUILD_BUG_ON(FUSE_IOMAP_OP_DONTCACHE	!= IOMAP_DONTCACHE);

	if (outarg->read_dev == FUSE_IOMAP_DEV_NULL) {
		ASSERT(outarg->read_dev != FUSE_IOMAP_DEV_NULL);
		return -EIO;
	}
	if (outarg->write_dev == FUSE_IOMAP_DEV_NULL) {
		ASSERT(outarg->write_dev != FUSE_IOMAP_DEV_NULL);
		return -EIO;
	}
	if (outarg->offset > pos) {
		ASSERT(outarg->offset <= pos);
		return -EIO;
	}
	if (outarg->length == 0) {
		ASSERT(outarg->length != 0);
		return -EIO;
	}
	if (outarg->offset + outarg->length <= pos) {
		ASSERT(outarg->offset + outarg->length > pos);
		return -EIO;
	}
	if (!fuse_iomap_check_type(outarg->write_type)) {
		ASSERT(fuse_iomap_check_type(outarg->write_type));
		return -EIO;
	}
	if (!fuse_iomap_check_flags(outarg->write_flags)) {
		ASSERT(fuse_iomap_check_flags(outarg->write_flags));
		return -EIO;
	}
	if (!fuse_iomap_check_type(outarg->read_type)) {
		ASSERT(fuse_iomap_check_type(outarg->read_type));
		return -EIO;
	}
	if (!fuse_iomap_check_flags(outarg->read_flags)) {
		ASSERT(fuse_iomap_check_flags(outarg->read_flags));
		return -EIO;
	}

	if (!(opflags & FUSE_IOMAP_OP_REPORT)) {
		/*
		 * XXX inline data reads and writes are not supported, how do
		 * we do this?
		 */
		ASSERT(outarg->read_type != FUSE_IOMAP_TYPE_INLINE);
		ASSERT(outarg->write_type != FUSE_IOMAP_TYPE_INLINE);

		if (outarg->read_type == FUSE_IOMAP_TYPE_INLINE)
			return -EIO;
		if (outarg->write_type == FUSE_IOMAP_TYPE_INLINE)
			return -EIO;
	}

	return 0;
}

static inline struct block_device *fuse_iomap_bdev(struct fuse_mount *fm,
						   unsigned int idx)
{
	struct fuse_conn *fc = fm->fc;
	struct file *file = NULL;

	spin_lock(&fc->lock);
	if (idx < fc->iomap_conn.nr_files)
		file = fc->iomap_conn.files[idx];
	spin_unlock(&fc->lock);

	if (!file)
		return NULL;

	if (!S_ISBLK(file_inode(file)->i_mode))
		return NULL;

	return I_BDEV(file->f_mapping->host);
}

static int fuse_iomap_begin(struct inode *inode, loff_t pos, loff_t count,
			    unsigned opflags, struct iomap *iomap,
			    struct iomap *srcmap)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_begin_in inarg = {
		.attr_ino = fi->orig_ino,
		.opflags = opflags,
		.pos = pos,
		.count = count,
	};
	struct fuse_iomap_begin_out outarg = { };
	struct fuse_mount *fm = get_fuse_mount(inode);
	struct block_device *read_bdev;
	FUSE_ARGS(args);
	int err;

	trace_fuse_iomap_begin(inode, pos, count, opflags);

	args.opcode = FUSE_IOMAP_BEGIN;
	args.nodeid = get_node_id(inode);
	args.in_numargs = 1;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	args.out_numargs = 1;
	args.out_args[0].size = sizeof(outarg);
	args.out_args[0].value = &outarg;
	err = fuse_simple_request(fm, &args);
	if (err) {
		trace_fuse_iomap_begin_error(inode, pos, count, opflags, err);
		return err;
	}

	trace_fuse_iomap_read_map(inode, &outarg);
	trace_fuse_iomap_write_map(inode, &outarg);

	err = fuse_iomap_validate(&outarg, opflags, pos);
	if (err)
		return err;

	read_bdev = fuse_iomap_bdev(fm, outarg.read_dev);
	if (!read_bdev)
		return -ENODEV;

	if ((opflags & IOMAP_WRITE) &&
	    outarg.write_type != FUSE_IOMAP_TYPE_PURE_OVERWRITE) {
		struct block_device *write_bdev =
			fuse_iomap_bdev(fm, outarg.write_dev);

		if (!write_bdev)
			return -ENODEV;

		/*
		 * For an out of place write, we must supply the write mapping
		 * via @iomap, and the read mapping via @srcmap.
		 */
		iomap->addr = outarg.write_addr;
		iomap->offset = outarg.offset;
		iomap->length = outarg.length;
		iomap->type = outarg.write_type;
		iomap->flags = outarg.write_flags;
		iomap->bdev = write_bdev;

		srcmap->addr = outarg.read_addr;
		srcmap->offset = outarg.offset;
		srcmap->length = outarg.length;
		srcmap->type = outarg.read_type;
		srcmap->flags = outarg.read_flags;
		srcmap->bdev = read_bdev;
	} else {
		/*
		 * For everything else (reads, reporting, and pure overwrites),
		 * we can return the sole mapping through @iomap and leave
		 * @srcmap unchanged from its default (HOLE).
		 */
		iomap->addr = outarg.read_addr;
		iomap->offset = outarg.offset;
		iomap->length = outarg.length;
		iomap->type = outarg.read_type;
		iomap->flags = outarg.read_flags;
		iomap->bdev = read_bdev;
	}

	return 0;
}

static bool fuse_want_iomap_end(const struct iomap *iomap, unsigned int opflags,
				loff_t count, ssize_t written)
{
	/* Caller demanded an iomap_end call. */
	if (iomap->flags & FUSE_IOMAP_F_WANT_IOMAP_END)
		return true;

	/* Reads and reporting should never affect the filesystem metadata */
	if (!(opflags & (IOMAP_WRITE | IOMAP_ZERO)))
		return false;

	/* Appending writes get an iomap_end call */
	if (iomap->flags & IOMAP_F_SIZE_CHANGED)
		return true;

	/* Short writes get an iomap_end call to clean up delalloc */
	return written < count;
}

static int fuse_iomap_end(struct inode *inode, loff_t pos, loff_t count,
			  ssize_t written, unsigned opflags,
			  struct iomap *iomap)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_end_in inarg = {
		.opflags = opflags,
		.attr_ino = fi->orig_ino,
		.pos = pos,
		.count = count,
		.written = written,

		.map_addr = iomap->addr,
		.map_length = iomap->length,
		.map_type = iomap->type,
		.map_flags = iomap->flags,
	};
	struct fuse_mount *fm = get_fuse_mount(inode);
	FUSE_ARGS(args);
	int err;

	if (!fuse_want_iomap_end(iomap, opflags, count, written))
		return 0;

	trace_fuse_iomap_end(inode, &inarg);

	args.opcode = FUSE_IOMAP_END;
	args.nodeid = get_node_id(inode);
	args.in_numargs = 1;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	err = fuse_simple_request(fm, &args);

	trace_fuse_iomap_end_error(inode, &inarg, err);

	return err;
}

const struct iomap_ops fuse_iomap_ops = {
	.iomap_begin		= fuse_iomap_begin,
	.iomap_end		= fuse_iomap_end,
};

static inline bool fuse_want_ioend(const struct fuse_iomap_ioend_in *inarg)
{
	/* Always send an ioend for errors. */
	if (inarg->error)
		return true;

	/* Send an ioend if we performed an IO involving metadata changes. */
	return inarg->written > 0 &&
	       (inarg->ioendflags & (FUSE_IOMAP_IOEND_SHARED |
				     FUSE_IOMAP_IOEND_UNWRITTEN |
				     FUSE_IOMAP_IOEND_APPEND));
}

static int fuse_iomap_ioend(struct inode *inode, loff_t pos, size_t written,
			    int error, unsigned ioendflags, sector_t new_addr)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_ioend_in inarg = {
		.ioendflags = ioendflags,
		.error = error,
		.attr_ino = fi->orig_ino,
		.pos = pos,
		.written = written,
		.new_addr = new_addr,
	};
	struct fuse_mount *fm = get_fuse_mount(inode);
	FUSE_ARGS(args);
	int err = 0;

	if (pos + written > i_size_read(inode))
		inarg.ioendflags |= FUSE_IOMAP_IOEND_APPEND;

	trace_fuse_iomap_ioend(inode, &inarg);

	if (!fuse_want_ioend(&inarg))
		goto out;

	args.opcode = FUSE_IOMAP_IOEND;
	args.nodeid = get_node_id(inode);
	args.in_numargs = 1;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	err = fuse_simple_request(fm, &args);

	trace_fuse_iomap_ioend_error(inode, &inarg, err);

	/*
	 * Preserve the original error code if userspace didn't respond or
	 * returned success despite the error we passed along via the ioend.
	 */
	if (error && (err == 0 || err == -ENOSYS))
		err = error;

out:
	/*
	 * If there weren't any ioend errors, update the incore isize, which
	 * confusingly takes the new i_size as "pos".
	 */
	if (!error && !err)
		fuse_write_update_attr(inode, pos + written, written);

	return err;
}

void fuse_iomap_conn_put(struct fuse_conn *fc)
{
	unsigned int i;

	for (i = 0; i < fc->iomap_conn.nr_files; i++) {
		struct file *file = fc->iomap_conn.files[i];

		trace_fuse_iomap_remove_dev(fc, i, file);

		fc->iomap_conn.files[i] = NULL;
		fput(file);
	}

	kfree(fc->iomap_conn.files);
	fc->iomap_conn.nr_files = 0;
}

/* Add a bdev to the fuse connection, returns the index or a negative errno */
static int __fuse_iomap_add_device(struct fuse_conn *fc, struct file *file)
{
	struct file **new_files;
	int ret;

	if (fc->iomap_conn.nr_files >= PAGE_SIZE / sizeof(unsigned int))
		return -EMFILE;

	new_files = krealloc_array(fc->iomap_conn.files,
				   fc->iomap_conn.nr_files + 1,
				   sizeof(struct file *),
				   GFP_KERNEL | __GFP_ZERO);
	if (!new_files)
		return -ENOMEM;

	spin_lock(&fc->lock);
	fc->iomap_conn.files = new_files;
	fc->iomap_conn.files[fc->iomap_conn.nr_files] = get_file(file);
	ret = fc->iomap_conn.nr_files++;
	spin_unlock(&fc->lock);

	trace_fuse_iomap_add_dev(fc, ret, file);

	return ret;
}

void fuse_iomap_init_reply(struct fuse_mount *fm)
{
	struct fuse_conn *fc = fm->fc;
	struct super_block *sb = fm->sb;

	if (sb->s_bdev)
		__fuse_iomap_add_device(fc, sb->s_bdev_file);

	if (fc->iomap_pagecache) {
		struct backing_dev_info *old_bdi = sb->s_bdi;
		char *suffix = sb->s_bdev ? "-fuseblk" : "-fuse";
		int err;

		/*
		 * sb->s_bdi points to the initial private bdi however we want
		 * to redirect it to a new private bdi with default dirty and
		 * readahead settings because iomap writeback won't be pushing
		 * a ton of dirty data through the fuse device
		 */
		sb->s_bdi = &noop_backing_dev_info;
		err = super_setup_bdi_name(sb, "%u:%u%s.iomap", MAJOR(fc->dev),
					   MINOR(fc->dev), suffix);
		if (err) {
			sb->s_bdi = old_bdi;
		} else {
			bdi_unregister(old_bdi);
			bdi_put(old_bdi);
		}
	}
}

int fuse_iomap_add_device(struct fuse_conn *fc,
			  const struct fuse_iomap_add_device_out *outarg)
{
	struct file *file;
	int ret;

	if (!fc->iomap)
		return -EINVAL;

	if (outarg->reserved)
		return -EINVAL;

	CLASS(fd, somefd)(outarg->fd);
	if (fd_empty(somefd))
		return -EBADF;
	file = fd_file(somefd);

	if (!S_ISBLK(file_inode(file)->i_mode))
		return -ENODEV;

	down_read(&fc->killsb);
	ret = __fuse_iomap_add_device(fc, file);
	up_read(&fc->killsb);
	if (ret < 0)
		return ret;

	return put_user(ret, outarg->map_dev);
}

int fuse_iomap_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		      u64 start, u64 count)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	int error;

	/*
	 * We are called directly from the vfs so we need to check per-inode
	 * support here explicitly.
	 */
	if (!fuse_has_iomap(inode))
		return -EOPNOTSUPP;

	if (fieinfo->fi_flags & FIEMAP_FLAG_XATTR)
		return -EOPNOTSUPP;

	if (fuse_is_bad(inode))
		return -EIO;

	if (!fuse_allow_current_process(fc))
		return -EACCES;

	trace_fuse_iomap_fiemap(inode, start, count, fieinfo->fi_flags);

	inode_lock_shared(inode);
	error = iomap_fiemap(inode, fieinfo, start, count,
			&fuse_iomap_ops);
	inode_unlock_shared(inode);

	return error;
}

sector_t fuse_iomap_bmap(struct address_space *mapping, sector_t block)
{
	ASSERT(fuse_has_iomap(mapping->host));

	return iomap_bmap(mapping, block, &fuse_iomap_ops);
}

loff_t fuse_iomap_lseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	struct fuse_conn *fc = get_fuse_conn(inode);

	ASSERT(fuse_has_iomap(inode));

	if (fuse_is_bad(inode))
		return -EIO;

	if (!fuse_allow_current_process(fc))
		return -EACCES;

	trace_fuse_iomap_lseek(inode, offset, whence);

	switch (whence) {
	case SEEK_HOLE:
		offset = iomap_seek_hole(inode, offset, &fuse_iomap_ops);
		break;
	case SEEK_DATA:
		offset = iomap_seek_data(inode, offset, &fuse_iomap_ops);
		break;
	default:
		return -ENOSYS;
	}

	if (offset < 0)
		return offset;
	return vfs_setpos(file, offset, inode->i_sb->s_maxbytes);
}

void fuse_iomap_open(struct inode *inode, struct file *file)
{
	if (fuse_has_iomap_direct_io(inode))
		file->f_mode |= FMODE_NOWAIT | FMODE_CAN_ODIRECT;
	if (fuse_has_iomap_pagecache(inode))
		file->f_mode |= FMODE_NOWAIT;
}

enum fuse_ilock_type {
	SHARED,
	EXCL,
};

static int fuse_iomap_ilock_iocb(const struct kiocb *iocb,
				 enum fuse_ilock_type type)
{
	struct inode *inode = file_inode(iocb->ki_filp);

	if (iocb->ki_flags & IOCB_NOWAIT) {
		switch (type) {
		case SHARED:
			return inode_trylock_shared(inode) ? 0 : -EAGAIN;
		case EXCL:
			return inode_trylock(inode) ? 0 : -EAGAIN;
		default:
			ASSERT(0);
			return -EIO;
		}
	} else {
		switch (type) {
		case SHARED:
			inode_lock_shared(inode);
			break;
		case EXCL:
			inode_lock(inode);
			break;
		default:
			ASSERT(0);
			return -EIO;
		}
	}

	return 0;
}

ssize_t fuse_iomap_direct_read(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	ASSERT(fuse_has_iomap_direct_io(inode));

	trace_fuse_iomap_direct_read(iocb, to);

	if (!iov_iter_count(to))
		return 0; /* skip atime */

	file_accessed(iocb->ki_filp);

	ret = fuse_iomap_ilock_iocb(iocb, SHARED);
	if (ret)
		return ret;
	ret = iomap_dio_rw(iocb, to, &fuse_iomap_ops, NULL, 0, NULL, 0);
	inode_unlock_shared(inode);

	trace_fuse_iomap_direct_read_end(iocb, to, ret);
	return ret;
}

static int fuse_iomap_dio_write_end_io(struct kiocb *iocb, ssize_t written,
				       int error, unsigned dioflags)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	unsigned int nofs_flag;
	unsigned int ioendflags = FUSE_IOMAP_IOEND_DIRECT;
	int ret;

	if (fuse_is_bad(inode))
		return -EIO;

	ASSERT(fuse_has_iomap_direct_io(inode));

	trace_fuse_iomap_dio_write_end_io(inode, iocb->ki_pos, written, error,
					  dioflags);

	if (dioflags & IOMAP_DIO_COW)
		ioendflags |= FUSE_IOMAP_IOEND_SHARED;
	if (dioflags & IOMAP_DIO_UNWRITTEN)
		ioendflags |= FUSE_IOMAP_IOEND_UNWRITTEN;

	/*
	 * We can allocate memory here while doing writeback on behalf of
	 * memory reclaim.  To avoid memory allocation deadlocks set the
	 * task-wide nofs context for the following operations.
	 */
	nofs_flag = memalloc_nofs_save();
	ret = fuse_iomap_ioend(inode, iocb->ki_pos, written, error, ioendflags,
			       FUSE_IOMAP_NULL_ADDR);
	memalloc_nofs_restore(nofs_flag);
	return ret;
}

static const struct iomap_dio_ops fuse_iomap_dio_write_ops = {
	.end_io		= fuse_iomap_dio_write_end_io,
};

static int fuse_iomap_direct_write_sync(struct kiocb *iocb, loff_t start,
					size_t count)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct fuse_conn *fc = get_fuse_conn(inode);
	loff_t end = start + count - 1;
	int err;

	/* Flush the file metadata, not the page cache. */
	err = sync_inode_metadata(inode, 1);
	if (err)
		return err;

	if (fc->no_fsync)
		return 0;

	err = fuse_fsync_common(iocb->ki_filp, start, end, iocb_is_dsync(iocb),
				FUSE_FSYNC);
	if (err == -ENOSYS) {
		fc->no_fsync = 1;
		err = 0;
	}
	return err;
}

static int
fuse_iomap_zero_range(
	struct inode		*inode,
	loff_t			pos,
	loff_t			len,
	bool			*did_zero)
{
	return iomap_zero_range(inode, pos, len, did_zero, &fuse_iomap_ops,
				NULL);
}

/* Take care of zeroing post-EOF blocks when they might exist. */
static ssize_t
fuse_iomap_write_zero_eof(
	struct kiocb		*iocb,
	struct iov_iter		*from,
	bool			*drained_dio)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct address_space *mapping = iocb->ki_filp->f_mapping;
	loff_t			isize;
	int			error;

	/*
	 * We need to serialise against EOF updates that occur in IO
	 * completions here. We want to make sure that nobody is changing the
	 * size while we do this check until we have placed an IO barrier (i.e.
	 * hold i_rwsem exclusively) that prevents new IO from being
	 * dispatched.  The spinlock effectively forms a memory barrier once we
	 * have i_rwsem exclusively so we are guaranteed to see the latest EOF
	 * value and hence be able to correctly determine if we need to run
	 * zeroing.
	 */
	spin_lock(&fi->lock);
	isize = i_size_read(inode);
	if (iocb->ki_pos <= isize) {
		spin_unlock(&fi->lock);
		return 0;
	}
	spin_unlock(&fi->lock);

	if (iocb->ki_flags & IOCB_NOWAIT)
		return -EAGAIN;

	if (!(*drained_dio)) {
		/*
		 * We now have an IO submission barrier in place, but AIO can
		 * do EOF updates during IO completion and hence we now need to
		 * wait for all of them to drain.  Non-AIO DIO will have
		 * drained before we are given the exclusive i_rwsem, and so
		 * for most cases this wait is a no-op.
		 */
		inode_dio_wait(inode);
		*drained_dio = true;
		return 1;
	}

	trace_fuse_iomap_write_zero_eof(iocb, from);

	filemap_invalidate_lock(mapping);
	error = fuse_iomap_zero_range(inode, isize, iocb->ki_pos - isize, NULL);
	filemap_invalidate_unlock(mapping);

	return error;
}

static ssize_t
fuse_iomap_write_checks(
	struct kiocb		*iocb,
	struct iov_iter		*from)
{
	struct inode		*inode = iocb->ki_filp->f_mapping->host;
	ssize_t			error;
	bool			drained_dio = false;

restart:
	error = generic_write_checks(iocb, from);
	if (error <= 0)
		return error;

	/*
	 * If the offset is beyond the size of the file, we need to zero all
	 * blocks that fall between the existing EOF and the start of this
	 * write.
	 *
	 * We can do an unlocked check for i_size here safely as I/O completion
	 * can only extend EOF.  Truncate is locked out at this point, so the
	 * EOF cannot move backwards, only forwards. Hence we only need to take
	 * the slow path when we are at or beyond the current EOF.
	 */
	if (fuse_has_iomap_pagecache(inode) &&
	    iocb->ki_pos > i_size_read(inode)) {
		error = fuse_iomap_write_zero_eof(iocb, from, &drained_dio);
		if (error == 1)
			goto restart;
		if (error)
			return error;
	}

	return kiocb_modified(iocb);
}

ssize_t fuse_iomap_direct_write(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t blockmask = i_blocksize(inode) - 1;
	loff_t pos = iocb->ki_pos;
	size_t count = iov_iter_count(from);
	bool was_dsync = false;
	ssize_t ret;

	ASSERT(fuse_has_iomap_direct_io(inode));

	trace_fuse_iomap_direct_write(iocb, from);

	/*
	 * direct I/O must be aligned to the fsblock size or we fall back to
	 * the old paths
	 */
	if ((iocb->ki_pos | count) & blockmask)
		return -ENOTBLK;

	/* fuse doesn't support S_SYNC, so complain if we see this. */
	if (IS_SYNC(inode)) {
		ASSERT(!IS_SYNC(inode));
		return -EIO;
	}

	/*
	 * Strip off IOCB_DSYNC so that we can run the fsync ourselves because
	 * we hold inode_lock; iomap_dio_rw calls generic_write_sync; and
	 * fuse_fsync tries to take inode_lock again.
	 */
	if (iocb_is_dsync(iocb)) {
		was_dsync = true;
		iocb->ki_flags &= ~IOCB_DSYNC;
	}

	ret = fuse_iomap_ilock_iocb(iocb, EXCL);
	if (ret)
		goto out_dsync;

	ret = fuse_iomap_write_checks(iocb, from);
	if (ret)
		goto out_unlock;

	ret = iomap_dio_rw(iocb, from, &fuse_iomap_ops,
			&fuse_iomap_dio_write_ops, 0, NULL, 0);
	if (ret)
		goto out_unlock;

	if (was_dsync) {
		/* Restore IOCB_DSYNC and call our sync function */
		iocb->ki_flags |= IOCB_DSYNC;
		ret = fuse_iomap_direct_write_sync(iocb, pos, count);
	}

out_unlock:
	inode_unlock(inode);
out_dsync:
	trace_fuse_iomap_direct_write_end(iocb, from, ret);
	if (was_dsync)
		iocb->ki_flags |= IOCB_DSYNC;
	return ret;
}

struct fuse_writepage_ctx {
	struct iomap_writepage_ctx ctx;
};

static void fuse_iomap_end_ioend(struct iomap_ioend *ioend)
{
	struct inode *inode = ioend->io_inode;
	unsigned int ioendflags = 0;
	unsigned int nofs_flag;
	int error = blk_status_to_errno(ioend->io_bio.bi_status);

	ASSERT(fuse_has_iomap_pagecache(inode));

	if (fuse_is_bad(inode))
		return;

	trace_fuse_iomap_end_ioend(ioend);

	if (ioend->io_flags & IOMAP_IOEND_SHARED)
		ioendflags |= FUSE_IOMAP_IOEND_SHARED;
	if (ioend->io_flags & IOMAP_IOEND_UNWRITTEN)
		ioendflags |= FUSE_IOMAP_IOEND_UNWRITTEN;

	/*
	 * We can allocate memory here while doing writeback on behalf of
	 * memory reclaim.  To avoid memory allocation deadlocks set the
	 * task-wide nofs context for the following operations.
	 */
	nofs_flag = memalloc_nofs_save();
	fuse_iomap_ioend(inode, ioend->io_offset, ioend->io_size, error,
			 ioendflags, FUSE_IOMAP_NULL_ADDR);
	iomap_finish_ioends(ioend, error);
	memalloc_nofs_restore(nofs_flag);
}

/*
 * Finish all pending IO completions that require transactional modifications.
 *
 * We try to merge physical and logically contiguous ioends before completion to
 * minimise the number of transactions we need to perform during IO completion.
 * Both unwritten extent conversion and COW remapping need to iterate and modify
 * one physical extent at a time, so we gain nothing by merging physically
 * discontiguous extents here.
 *
 * The ioend chain length that we can be processing here is largely unbound in
 * length and we may have to perform significant amounts of work on each ioend
 * to complete it. Hence we have to be careful about holding the CPU for too
 * long in this loop.
 */
static void fuse_iomap_end_io(struct work_struct *work)
{
	struct fuse_inode *fi =
		container_of(work, struct fuse_inode, ioend_work);
	struct iomap_ioend *ioend;
	struct list_head tmp;
	unsigned long flags;

	spin_lock_irqsave(&fi->ioend_lock, flags);
	list_replace_init(&fi->ioend_list, &tmp);
	spin_unlock_irqrestore(&fi->ioend_lock, flags);

	iomap_sort_ioends(&tmp);
	while ((ioend = list_first_entry_or_null(&tmp, struct iomap_ioend,
			io_list))) {
		list_del_init(&ioend->io_list);
		iomap_ioend_try_merge(ioend, &tmp);
		fuse_iomap_end_ioend(ioend);
		cond_resched();
	}
}

static void fuse_iomap_end_bio(struct bio *bio)
{
	struct iomap_ioend *ioend = iomap_ioend_from_bio(bio);
	struct inode *inode = ioend->io_inode;
	struct fuse_inode *fi = get_fuse_inode(inode);
	unsigned long flags;

	ASSERT(fuse_has_iomap_pagecache(inode));

	spin_lock_irqsave(&fi->ioend_lock, flags);
	if (list_empty(&fi->ioend_list))
		WARN_ON_ONCE(!queue_work(system_unbound_wq, &fi->ioend_work));
	list_add_tail(&ioend->io_list, &fi->ioend_list);
	spin_unlock_irqrestore(&fi->ioend_lock, flags);
}

/*
 * Fast revalidation of the cached writeback mapping. Return true if the current
 * mapping is valid, false otherwise.
 */
static bool fuse_iomap_revalidate_writeback(struct iomap_writepage_ctx *wpc,
					    loff_t offset)
{
	if (offset < wpc->iomap.offset ||
	    offset >= wpc->iomap.offset + wpc->iomap.length)
		return false;

	/* XXX actually use revalidation cookie */
	return true;
}

static int fuse_iomap_map_blocks(struct iomap_writepage_ctx *wpc,
				 struct inode *inode, loff_t offset,
				 unsigned int len)
{
	struct iomap write_iomap, dontcare;
	int ret;

	if (fuse_is_bad(inode))
		return -EIO;

	ASSERT(fuse_has_iomap_pagecache(inode));

	trace_fuse_iomap_map_blocks(inode, offset, len);

	if (fuse_iomap_revalidate_writeback(wpc, offset))
		return 0;

	/* Pretend that this is a directio write */
	ret = fuse_iomap_begin(inode, offset, len, IOMAP_DIRECT | IOMAP_WRITE,
			       &write_iomap, &dontcare);
	if (ret)
		return ret;

	/*
	 * Landed in a hole or beyond EOF?  Send that to iomap, it'll skip
	 * writing back the file range.
	 */
	if (write_iomap.offset > offset) {
		write_iomap.length = write_iomap.offset - offset;
		write_iomap.offset = offset;
		write_iomap.type = IOMAP_HOLE;
	}

	memcpy(&wpc->iomap, &write_iomap, sizeof(struct iomap));
	return 0;
}

static int fuse_iomap_submit_ioend(struct iomap_writepage_ctx *wpc, int status)
{
	struct iomap_ioend *ioend = wpc->ioend;

	ASSERT(fuse_has_iomap_pagecache(ioend->io_inode));

	trace_fuse_iomap_submit_ioend(ioend->io_inode, wpc->nr_folios, status);

	/* always call our ioend function, even if we cancel the bio */
	ioend->io_bio.bi_end_io = fuse_iomap_end_bio;

	if (status)
		return status;
	submit_bio(&ioend->io_bio);
	return 0;
}

/*
 * If the folio has delalloc blocks on it, the caller is asking us to punch them
 * out. If we don't, we can leave a stale delalloc mapping covered by a clean
 * page that needs to be dirtied again before the delalloc mapping can be
 * converted. This stale delalloc mapping can trip up a later direct I/O read
 * operation on the same region.
 *
 * We prevent this by truncating away the delalloc regions on the folio. Because
 * they are delalloc, we can do this without needing a transaction. Indeed - if
 * we get ENOSPC errors, we have to be able to do this truncation without a
 * transaction as there is no space left for block reservation (typically why
 * we see a ENOSPC in writeback).
 */
static void fuse_iomap_discard_folio(struct folio *folio, loff_t pos)
{
	struct inode *inode = folio->mapping->host;
	struct fuse_inode *fi = get_fuse_inode(inode);

	if (fuse_is_bad(inode))
		return;

	ASSERT(fuse_has_iomap_pagecache(inode));

	trace_fuse_iomap_discard_folio(inode, pos, folio_size(folio));

	printk_ratelimited(KERN_ERR
		"page discard on page %px, inode 0x%llx, pos %llu.",
			folio, fi->orig_ino, pos);

	/* XXX actually punch the new delalloc ranges? */
}

static const struct iomap_writeback_ops fuse_iomap_writeback_ops = {
	.map_blocks		= fuse_iomap_map_blocks,
	.submit_ioend		= fuse_iomap_submit_ioend,
	.discard_folio		= fuse_iomap_discard_folio,
};

static int fuse_iomap_writepages(struct address_space *mapping,
				 struct writeback_control *wbc)
{
	struct fuse_writepage_ctx wpc = { };

	ASSERT(fuse_has_iomap_pagecache(mapping->host));

	trace_fuse_iomap_writepages(mapping->host, wbc);

	return iomap_writepages(mapping, wbc, &wpc.ctx,
				&fuse_iomap_writeback_ops);
}

static int fuse_iomap_read_folio(struct file *file, struct folio *folio)
{
	ASSERT(fuse_has_iomap_pagecache(file_inode(file)));

	trace_fuse_iomap_read_folio(folio);

	return iomap_read_folio(folio, &fuse_iomap_ops);
}

static void fuse_iomap_readahead(struct readahead_control *rac)
{
	ASSERT(fuse_has_iomap_pagecache(file_inode(rac->file)));

	trace_fuse_iomap_readahead(rac);

	iomap_readahead(rac, &fuse_iomap_ops);
}

const struct address_space_operations fuse_iomap_aops = {
	.read_folio		= fuse_iomap_read_folio,
	.readahead		= fuse_iomap_readahead,
	.writepages		= fuse_iomap_writepages,
	.dirty_folio		= iomap_dirty_folio,
	.release_folio		= iomap_release_folio,
	.invalidate_folio	= iomap_invalidate_folio,
	.migrate_folio		= filemap_migrate_folio,
	.is_partially_uptodate  = iomap_is_partially_uptodate,
	.error_remove_folio	= generic_error_remove_folio,

	/* These aren't pagecache operations per se */
	.bmap			= fuse_bmap,
	.direct_IO		= fuse_direct_IO,
};

void fuse_iomap_init_pagecache(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	unsigned int min_order = 0;

	ASSERT(fuse_has_iomap(inode));

	/* Manage timestamps ourselves, don't make the fuse server do it */
	inode->i_flags &= ~S_NOCMTIME;
	inode->i_flags &= ~S_NOATIME;
	inode->i_data.a_ops = &fuse_iomap_aops;

	INIT_WORK(&fi->ioend_work, fuse_iomap_end_io);
	INIT_LIST_HEAD(&fi->ioend_list);
	spin_lock_init(&fi->ioend_lock);

	if (inode->i_blkbits > PAGE_SHIFT)
		min_order = inode->i_blkbits - PAGE_SHIFT;

	mapping_set_folio_min_order(inode->i_mapping, min_order);
}

void fuse_iomap_destroy_pagecache(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);

	ASSERT(fuse_has_iomap(inode));
	ASSERT(list_empty(&fi->ioend_list));
}

/*
 * Locking for serialisation of IO during page faults. This results in a lock
 * ordering of:
 *
 * mmap_lock (MM)
 *   sb_start_pagefault(vfs, freeze)
 *     invalidate_lock (vfs - truncate serialisation)
 *       page_lock (MM)
 *         i_lock (FUSE - extent map serialisation)
 */
static vm_fault_t fuse_iomap_page_mkwrite(struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	vm_fault_t ret;

	ASSERT(fuse_has_iomap_pagecache(inode));

	trace_fuse_iomap_page_mkwrite(vmf);

	sb_start_pagefault(inode->i_sb);
	file_update_time(vmf->vma->vm_file);

	filemap_invalidate_lock_shared(mapping);
	ret = iomap_page_mkwrite(vmf, &fuse_iomap_ops, NULL);
	filemap_invalidate_unlock_shared(mapping);

	sb_end_pagefault(inode->i_sb);
	return ret;
}

static const struct vm_operations_struct fuse_iomap_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= fuse_iomap_page_mkwrite,
};

int fuse_iomap_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(file);

	ASSERT(fuse_has_iomap_pagecache(inode));

	file_accessed(file);
	vma->vm_ops = &fuse_iomap_vm_ops;
	return 0;
}

ssize_t fuse_iomap_buffered_read(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	ASSERT(fuse_has_iomap_pagecache(inode));

	trace_fuse_iomap_buffered_read(iocb, to);

	if (!iov_iter_count(to))
		return 0; /* skip atime */

	file_accessed(iocb->ki_filp);

	ret = fuse_iomap_ilock_iocb(iocb, SHARED);
	if (ret)
		return ret;
	ret = generic_file_read_iter(iocb, to);
	inode_unlock_shared(inode);

	trace_fuse_iomap_buffered_read_end(iocb, to, ret);
	return ret;
}

ssize_t fuse_iomap_buffered_write(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct fuse_inode *fi = get_fuse_inode(inode);
	loff_t pos = iocb->ki_pos;
	ssize_t ret;

	ASSERT(fuse_has_iomap_pagecache(inode));

	trace_fuse_iomap_buffered_write(iocb, from);

	ret = fuse_iomap_ilock_iocb(iocb, EXCL);
	if (ret)
		return ret;

	ret = fuse_iomap_write_checks(iocb, from);
	if (ret)
		goto out_unlock;

	if (inode->i_size < pos + iov_iter_count(from))
		set_bit(FUSE_I_SIZE_UNSTABLE, &fi->state);

	ret = iomap_file_buffered_write(iocb, from, &fuse_iomap_ops, NULL);

	if (ret > 0)
		fuse_write_update_attr(inode, pos + ret, ret);
	clear_bit(FUSE_I_SIZE_UNSTABLE, &fi->state);

out_unlock:
	inode_unlock(inode);

	if (ret > 0) {
		/* Handle various SYNC-type writes */
		ret = generic_write_sync(iocb, ret);
	}
	trace_fuse_iomap_buffered_write_end(iocb, from, ret);
	return ret;
}

static int
fuse_iomap_truncate_page(
	struct inode *inode,
	loff_t			pos,
	bool			*did_zero)
{
	return iomap_truncate_page(inode, pos, did_zero, &fuse_iomap_ops,
				   NULL);
}
/*
 * Truncate file.  Must have write permission and not be a directory.
 *
 * Caution: The caller of this function is responsible for calling
 * setattr_prepare() or otherwise verifying the change is fine.
 */
static int
fuse_iomap_setattr_size(
	struct mnt_idmap	*idmap,
	struct dentry		*dentry,
	struct inode *inode,
	struct iattr		*iattr)
{
	loff_t oldsize, newsize;
	int			error;
	bool			did_zeroing = false;

	//xfs_assert_ilocked(ip, XFS_IOLOCK_EXCL | XFS_MMAPLOCK_EXCL);
	ASSERT(S_ISREG(inode->i_mode));
	ASSERT((iattr->ia_valid & (ATTR_UID|ATTR_GID|ATTR_ATIME|ATTR_ATIME_SET|
		ATTR_MTIME_SET|ATTR_TIMES_SET)) == 0);

	oldsize = inode->i_size;
	newsize = iattr->ia_size;

	/*
	 * Wait for all direct I/O to complete.
	 */
	inode_dio_wait(inode);

	/*
	 * File data changes must be complete and flushed to disk before we
	 * call userspace to modify the inode.
	 *
	 * Start with zeroing any data beyond EOF that we may expose on file
	 * extension, or zeroing out the rest of the block on a downward
	 * truncate.
	 */
	if (newsize > oldsize) {
		trace_fuse_iomap_truncate_up(inode, oldsize, newsize - oldsize);

		error = fuse_iomap_zero_range(inode, oldsize, newsize - oldsize,
					      &did_zeroing);
	} else {
		trace_fuse_iomap_truncate_down(inode, newsize,
					       oldsize - newsize);

		error = fuse_iomap_truncate_page(inode, newsize, &did_zeroing);
	}
	if (error)
		return error;

	/*
	 * We've already locked out new page faults, so now we can safely
	 * remove pages from the page cache knowing they won't get refaulted
	 * until we drop the mapping invalidation lock after the extent
	 * manipulations are complete. The truncate_setsize() call also cleans
	 * folios spanning EOF on extending truncates and hence ensures
	 * sub-page block size filesystems are correctly handled, too.
	 *
	 * And we update in-core i_size and truncate page cache beyond newsize
	 * before writing back the whole file, so we're guaranteed not to write
	 * stale data past the new EOF on truncate down.
	 */
	truncate_setsize(inode, newsize);

	/*
	 * We are going to tell userspace to log the inode size change so any
	 * previous writes that are beyond the on disk EOF and the new EOF that
	 * have not been written out need to be written here.  If we do not
	 * write the data out, we expose ourselves to the null files problem.
	 * Note that this includes any block zeroing we did above; otherwise
	 * those blocks may not be zeroed after a crash.  It's really clumsy
	 * to flush the entire file, but we don't know the ondisk inode size
	 * so we use a big hammer instead.
	 */
	if (did_zeroing || newsize > 0) {
		error = filemap_write_and_wait(inode->i_mapping);
		if (error)
			return error;
	}

	return 0;
}

int
fuse_iomap_setsize(
	struct mnt_idmap	*idmap,
	struct dentry		*dentry,
	struct iattr		*iattr)
{
	struct inode *inode = d_inode(dentry);
	int error;

	ASSERT(fuse_has_iomap(inode));
	ASSERT(fuse_has_iomap_pagecache(inode));

	trace_fuse_iomap_setsize(inode, iattr->ia_size, 0);

	error = inode_newsize_ok(inode, iattr->ia_size);
	if (error)
		return error;
	return fuse_iomap_setattr_size(idmap, dentry, inode, iattr);
}

static int fuse_iomap_punch_range(struct inode *inode, loff_t offset,
				  loff_t length)
{
	loff_t isize = i_size_read(inode);
	int error;

	trace_fuse_iomap_punch_range(inode, offset, length);

	/*
	 * Now that we've unmap all full blocks we'll have to zero out any
	 * partial block at the beginning and/or end.  iomap_zero_range is
	 * smart enough to skip holes and unwritten extents, including those we
	 * just created, but we must take care not to zero beyond EOF, which
	 * would enlarge i_size.
	 */
	if (offset >= isize)
		return 0;
	if (offset + length > isize)
		length = isize - offset;
	error = fuse_iomap_zero_range(inode, offset, length, NULL);
	if (error)
		return error;

	/*
	 * If we zeroed right up to EOF and EOF straddles a page boundary we
	 * must make sure that the post-EOF area is also zeroed because the
	 * page could be mmap'd and iomap_zero_range doesn't do that for us.
	 * Writeback of the eof page will do this, albeit clumsily.
	 */
	if (offset + length >= isize && offset_in_page(offset + length) > 0) {
		error = filemap_write_and_wait_range(inode->i_mapping,
					round_down(offset + length, PAGE_SIZE),
					LLONG_MAX);
	}

	return error;
}

int
fuse_iomap_fallocate(
	struct file		*file,
	int			mode,
	loff_t			offset,
	loff_t			length,
	loff_t			new_size)
{
	struct inode *inode = file_inode(file);
	int error;

	ASSERT(fuse_has_iomap(inode));
	ASSERT(fuse_has_iomap_pagecache(inode));

	trace_fuse_iomap_fallocate(inode, mode, offset, length, new_size);

	/*
	 * If we unmapped blocks from the file range, then we zero the
	 * pagecache for those regions and push them to disk rather than make
	 * the fuse server manually zero the disk blocks.
	 */
	if (mode & (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE)) {
		error = fuse_iomap_punch_range(inode, offset, length);
		if (error)
			return error;
	}

	/*
	 * If this is an extending write, we need to zero the bytes beyond the
	 * new EOF.
	 */
	if (new_size) {
		struct iattr iattr = {
			.ia_valid	= ATTR_SIZE,
			.ia_size	= new_size,
		};

		return fuse_iomap_setsize(file_mnt_idmap(file),
					  file_dentry(file), &iattr);
	}

	return 0;
}
