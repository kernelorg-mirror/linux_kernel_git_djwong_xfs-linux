// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org.
 */
#include "fuse_i.h"
#include "fuse_trace.h"
#include <linux/iomap.h>

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
