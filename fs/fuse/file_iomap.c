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
# define ASSERT(a)		do { WARN(!(a), "Assertion failed: %s, func: %s, line: %d", #a, __func__, __LINE__); } while (0)
# define BAD_DATA(condition)	(WARN(condition, "Bad mapping: %s, func: %s, line: %d", #condition, __func__, __LINE__))
#else
# define ASSERT(a)
# define BAD_DATA(condition)	(condition)
#endif

bool fuse_iomap_enabled(void)
{
	/*
	 * There are fears that a fuse+iomap server could somehow DoS the
	 * system by doing things like going out to lunch during a writeback
	 * related iomap request.  Only allow iomap access if the fuse server
	 * has rawio capabilities since those processes can mess things up
	 * quite well even without our help.
	 */
	return enable_iomap && has_capability_noaudit(current, CAP_SYS_RAWIO);
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
static inline int
fuse_iomap_begin_validate(const struct fuse_iomap_begin_out *outarg,
			  const struct inode *inode,
			  unsigned opflags, loff_t pos)
{
	const unsigned int blocksize = i_blocksize(inode);
	uint64_t end;

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

	/* No garbage mapping types or flags */
	if (BAD_DATA(!fuse_iomap_check_type(outarg->read_type)))
		return -EIO;
	if (BAD_DATA(!fuse_iomap_check_flags(outarg->read_flags)))
		return -EIO;

	if (BAD_DATA(!fuse_iomap_check_type(outarg->write_type)))
		return -EIO;
	if (BAD_DATA(!fuse_iomap_check_flags(outarg->write_flags)))
		return -EIO;

	/*
	 * Must have returned a mapping for at least the first byte in the
	 * range.
	 */
	if (BAD_DATA(outarg->offset > pos))
		return -EIO;
	if (BAD_DATA(outarg->length == 0))
		return -EIO;

	/* File range must be aligned to blocksize */
	if (BAD_DATA(!IS_ALIGNED(outarg->offset, blocksize)))
		return -EIO;
	if (BAD_DATA(!IS_ALIGNED(outarg->length, blocksize)))
		return -EIO;

	/* No overflows in the file range */
	if (BAD_DATA(check_add_overflow(outarg->offset, outarg->length, &end)))
		return -EIO;
	if (BAD_DATA(end <= pos))
		return -EIO;

	/* File range cannot start past maxbytes */
	if (BAD_DATA(outarg->offset >= inode->i_sb->s_maxbytes))
		return -EIO;

	switch (outarg->read_type) {
	case FUSE_IOMAP_TYPE_PURE_OVERWRITE:
		/* "Pure overwrite" only allowed for write mapping */
		BAD_DATA(outarg->read_type == FUSE_IOMAP_TYPE_PURE_OVERWRITE);
		return -EIO;
	case FUSE_IOMAP_TYPE_MAPPED:
	case FUSE_IOMAP_TYPE_UNWRITTEN:
		/* Mappings backed by space must have a device/addr */
		if (BAD_DATA(outarg->read_dev == FUSE_IOMAP_DEV_NULL))
			return -EIO;
		if (BAD_DATA(outarg->read_addr == FUSE_IOMAP_NULL_ADDR))
			return -EIO;
		break;
	case FUSE_IOMAP_TYPE_DELALLOC:
	case FUSE_IOMAP_TYPE_HOLE:
	case FUSE_IOMAP_TYPE_INLINE:
		/* Mappings not backed by space cannot have a device addr. */
		if (BAD_DATA(outarg->read_dev != FUSE_IOMAP_DEV_NULL))
			return -EIO;
		if (BAD_DATA(outarg->read_addr != FUSE_IOMAP_NULL_ADDR))
			return -EIO;
		break;
	default:
		/* should have been caught already */
		return -EIO;
	}

	switch (outarg->write_type) {
	case FUSE_IOMAP_TYPE_MAPPED:
	case FUSE_IOMAP_TYPE_UNWRITTEN:
		/* Mappings backed by space must have a device/addr */
		if (BAD_DATA(outarg->write_dev == FUSE_IOMAP_DEV_NULL))
			return -EIO;
		if (BAD_DATA(outarg->write_addr == FUSE_IOMAP_NULL_ADDR))
			return -EIO;
		break;
	case FUSE_IOMAP_TYPE_PURE_OVERWRITE:
	case FUSE_IOMAP_TYPE_HOLE:
	case FUSE_IOMAP_TYPE_DELALLOC:
	case FUSE_IOMAP_TYPE_INLINE:
		/* Mappings not backed by space cannot have a device addr. */
		if (BAD_DATA(outarg->write_dev != FUSE_IOMAP_DEV_NULL))
			return -EIO;
		if (BAD_DATA(outarg->write_addr != FUSE_IOMAP_NULL_ADDR))
			return -EIO;
		break;
	default:
		/* should have been caught already */
		return -EIO;
	}

	/* No overflows in the device range, if supplied */
	if (outarg->read_addr != FUSE_IOMAP_NULL_ADDR &&
	    BAD_DATA(check_add_overflow(outarg->read_addr, outarg->length, &end)))
		return -EIO;

	if (outarg->write_addr != FUSE_IOMAP_NULL_ADDR &&
	    BAD_DATA(check_add_overflow(outarg->write_addr, outarg->length, &end)))
		return -EIO;

	if (!(opflags & FUSE_IOMAP_OP_REPORT)) {
		/*
		 * XXX inline data reads and writes are not supported, how do
		 * we do this?
		 */
		if (BAD_DATA(outarg->read_type == FUSE_IOMAP_TYPE_INLINE))
			return -EIO;
		if (BAD_DATA(outarg->write_type == FUSE_IOMAP_TYPE_INLINE))
			return -EIO;
	}

	return 0;
}

static inline bool fuse_is_iomap_file_write(unsigned int opflags)
{
	return opflags & (IOMAP_WRITE | IOMAP_ZERO | IOMAP_UNSHARE);
}

static struct fuse_iomap_dev *fuse_iomap_dev_get(struct fuse_iomap_dev *fb)
{
	if (fb && refcount_inc_not_zero(&fb->count))
		return fb;
	return NULL;
}

static void fuse_iomap_dev_free(struct fuse_iomap_dev *fb)
{
	if (fb->file)
		fput(fb->file);
	kfree_rcu(fb, rcu);
}

static void fuse_iomap_dev_put(struct fuse_iomap_dev *fb)
{
	if (fb && refcount_dec_and_test(&fb->count))
		fuse_iomap_dev_free(fb);
}

static int fuse_iomap_dev_id_alloc(struct fuse_conn *fc,
				   struct fuse_iomap_dev *fb)
{
	int id;

	idr_preload(GFP_KERNEL);
	spin_lock(&fc->lock);
	id = idr_alloc_cyclic(&fc->iomap_conn.device_map, fb, 1, 0,
			      GFP_ATOMIC);
	spin_unlock(&fc->lock);
	idr_preload_end();

	trace_fuse_iomap_add_dev(fc, id, fb);

	return id;
}

static struct fuse_iomap_dev *fuse_iomap_dev_id_remove(struct fuse_conn *fc,
						       int id)
{
	struct fuse_iomap_dev *fb;

	spin_lock(&fc->lock);
	fb = idr_remove(&fc->iomap_conn.device_map, id);
	spin_unlock(&fc->lock);

	if (fb)
		trace_fuse_iomap_remove_dev(fc, id, fb);

	return fb;
}

static inline struct fuse_iomap_dev *
fuse_iomap_dev_id_find(struct fuse_conn *fc, int idx)
{
	struct fuse_iomap_dev *fb;

	rcu_read_lock();
	fb = idr_find(&fc->iomap_conn.device_map, idx);
	fb = fuse_iomap_dev_get(fb);
	rcu_read_unlock();

	return fb;
}

static inline struct fuse_iomap_dev *
fuse_iomap_find_dev(struct fuse_conn *fc, uint16_t map_type, uint32_t map_dev)
{
	struct fuse_iomap_dev *ret = NULL;

	if (map_dev != FUSE_IOMAP_DEV_NULL && map_dev < INT_MAX)
		ret = fuse_iomap_dev_id_find(fc, map_dev);

	switch (map_type) {
	case FUSE_IOMAP_TYPE_MAPPED:
	case FUSE_IOMAP_TYPE_UNWRITTEN:
		/* Mappings backed by space must have a device/addr */
		if (BAD_DATA(ret == NULL))
			return ERR_PTR(-EIO);
		break;
	}

	return ret;
}

static inline void
fuse_iomap_set_device(struct iomap *iomap, const struct fuse_iomap_dev *fb)
{
	iomap->bdev = fb ? fb->bdev : NULL;
	iomap->dax_dev = NULL;
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
	struct fuse_iomap_dev *read_dev = NULL;
	struct fuse_iomap_dev *write_dev = NULL;
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

	err = fuse_iomap_begin_validate(&outarg, inode, opflags, pos);
	if (err)
		return err;

	read_dev = fuse_iomap_find_dev(fm->fc, outarg.read_type,
				       outarg.read_dev);
	if (IS_ERR(read_dev))
		return PTR_ERR(read_dev);

	if (fuse_is_iomap_file_write(opflags) &&
	    outarg.write_type != FUSE_IOMAP_TYPE_PURE_OVERWRITE) {

		write_dev = fuse_iomap_find_dev(fm->fc, outarg.write_type,
						outarg.write_dev);
		if (IS_ERR(write_dev)) {
			err = PTR_ERR(write_dev);
			goto out_read_dev;
		}

		/*
		 * For an out of place write, we must supply the write mapping
		 * via @iomap, and the read mapping via @srcmap.
		 */
		iomap->addr = outarg.write_addr;
		iomap->offset = outarg.offset;
		iomap->length = outarg.length;
		iomap->type = outarg.write_type;
		iomap->flags = outarg.write_flags;
		fuse_iomap_set_device(iomap, write_dev);

		srcmap->addr = outarg.read_addr;
		srcmap->offset = outarg.offset;
		srcmap->length = outarg.length;
		srcmap->type = outarg.read_type;
		srcmap->flags = outarg.read_flags;
		fuse_iomap_set_device(srcmap, read_dev);
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
		fuse_iomap_set_device(iomap, read_dev);
	}

	/*
	 * XXX: if we ever want to support closing devices, we need a way to 
	 * track the fuse_iomap_dev refcount all the way through bio endios.
	 * For now we put the refcount here because you can't remove an iomap
	 * device until unmount time.
	 */
	fuse_iomap_dev_put(write_dev);
out_read_dev:
	fuse_iomap_dev_put(read_dev);
	return err;
}

static bool fuse_want_iomap_end(const struct iomap *iomap, unsigned int opflags,
				loff_t count, ssize_t written)
{
	/* Caller demanded an iomap_end call. */
	if (iomap->flags & FUSE_IOMAP_F_WANT_IOMAP_END)
		return true;

	/* Reads and reporting should never affect the filesystem metadata */
	if (!fuse_is_iomap_file_write(opflags))
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

int fuse_iomap_conn_alloc(struct fuse_conn *fc)
{
	idr_init(&fc->iomap_conn.device_map);
	return 0;
}

static int fuse_iomap_dev_id_free(int id, void *p, void *data)
{
	struct fuse_iomap_dev *fb = p;
	struct fuse_conn *fc = data;

	trace_fuse_iomap_remove_dev(fc, id, fb);

	WARN_ON_ONCE(refcount_read(&fb->count) != 1);
	fuse_iomap_dev_free(fb);
	return 0;
}

void fuse_iomap_conn_put(struct fuse_conn *fc)
{
	idr_for_each(&fc->iomap_conn.device_map, fuse_iomap_dev_id_free, fc);
	idr_destroy(&fc->iomap_conn.device_map);
}

static struct fuse_iomap_dev *fuse_iomap_dev_alloc(struct file *file)
{
	struct fuse_iomap_dev *fb =
			kmalloc(sizeof(struct fuse_iomap_dev), GFP_KERNEL);

	if (!fb)
		return NULL;

	fb->file = file;
	fb->bdev = I_BDEV(file->f_mapping->host);
	refcount_set(&fb->count, 1);

	return fb;
}

bool fuse_iomap_fill_super(struct fuse_mount *fm)
{
	struct fuse_conn *fc = fm->fc;
	struct super_block *sb = fm->sb;
	int res;

	if (sb->s_bdev) {
		/*
		 * Try to install s_bdev as the first iomap device, if this
		 * is a block-device filesystem.
		 */
		struct fuse_iomap_dev *fb =
					fuse_iomap_dev_alloc(sb->s_bdev_file);

		if (!fb)
			return false;

		res = fuse_iomap_dev_id_alloc(fc, fb);
		if (res < 0)
			return false;
		if (res != 1) {
			struct fuse_iomap_dev *bad =
					fuse_iomap_dev_id_remove(fc, res);

			ASSERT(res == 1);
			ASSERT(bad == fb);
			fuse_iomap_dev_put(bad);
			return false;
		}
	}

	/*
	 * Enable syncfs for iomap fuse servers so that we can send a final
	 * flush at unmount time.  This also means that we can support
	 * freeze/thaw properly.
	 */
	fc->sync_fs = true;
	return true;
}

int fuse_iomap_dev_add(struct fuse_conn *fc, const struct fuse_backing_map *map)
{
	struct file *file;
	struct fuse_iomap_dev *fb = NULL;
	int res;

	trace_fuse_iomap_dev_add(fc, map);

	res = -EPERM;
	if (!fc->iomap)
		goto out;

	res = -EINVAL;
	if (map->flags || map->padding)
		goto out;

	file = fget_raw(map->fd);
	res = -EBADF;
	if (!file)
		goto out;

	res = -ENODEV;
	if (!S_ISBLK(file_inode(file)->i_mode))
		goto out_fput;

	fb = fuse_iomap_dev_alloc(file);
	if (!fb)
		goto out_fput;

	res = fuse_iomap_dev_id_alloc(fc, fb);
	if (res < 0) {
		fuse_iomap_dev_free(fb);
		goto out;
	}

	return res;

out_fput:
	fput(file);
out:
	return res;
}

void fuse_iomap_conn_destroy(struct fuse_mount *fm)
{
	struct fuse_conn *fc = fm->fc;

	/*
	 * Flush all pending commands, syncfs, flush that, and send a destroy
	 * command.  This gives the fuse server a chance to process all the
	 * pending releases, write the last bits of metadata changes to disk,
	 * and close the iomap block devices before we return from the umount
	 * call.  The caller already flushed previously pending requests, so we
	 * only need the flush to wait for syncfs.
	 */
	sync_filesystem(fm->sb);
	fuse_flush_requests(fc, 60 * HZ);
	fuse_send_destroy(fm);
}
