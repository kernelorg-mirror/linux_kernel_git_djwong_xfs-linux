// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <linux/iomap.h>
#include "fuse_i.h"
#include "fuse_trace.h"
#include "iomap_i.h"

#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG_DEFAULT)
DEFINE_STATIC_KEY_TRUE(fuse_iomap_debug);
#else
DEFINE_STATIC_KEY_FALSE(fuse_iomap_debug);
#endif

static bool __read_mostly enable_iomap =
#if IS_ENABLED(CONFIG_FUSE_IOMAP_BY_DEFAULT)
	true;
#else
	false;
#endif
module_param(enable_iomap, bool, 0644);
MODULE_PARM_DESC(enable_iomap, "Enable file I/O through iomap");

#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG)
static struct kobject *iomap_kobj;

static ssize_t fuse_iomap_debug_show(struct kobject *kobject,
				     struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!static_key_enabled(&fuse_iomap_debug));
}

static ssize_t fuse_iomap_debug_store(struct kobject *kobject,
				      struct kobj_attribute *a,
				      const char *buf, size_t count)
{
	int ret;
	int val;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	if (val < 0 || val > 1)
		return -EINVAL;

	if (val)
		static_branch_enable(&fuse_iomap_debug);
	else
		static_branch_disable(&fuse_iomap_debug);

	return count;
}

#define __INIT_KOBJ_ATTR(_name, _mode, _show, _store)			\
{									\
	.attr	= { .name = __stringify(_name), .mode = _mode },	\
	.show	= _show,						\
	.store	= _store,						\
}

#define FUSE_ATTR_RW(_name, _show, _store)			\
	static struct kobj_attribute fuse_attr_##_name =	\
			__INIT_KOBJ_ATTR(_name, 0644, _show, _store)

#define FUSE_ATTR_PTR(_name)					\
	(&fuse_attr_##_name.attr)

FUSE_ATTR_RW(debug, fuse_iomap_debug_show, fuse_iomap_debug_store);

static const struct attribute *fuse_iomap_attrs[] = {
	FUSE_ATTR_PTR(debug),
	NULL,
};

int fuse_iomap_sysfs_init(struct kobject *fuse_kobj)
{
	int error;

	iomap_kobj = kobject_create_and_add("iomap", fuse_kobj);
	if (!iomap_kobj)
		return -ENOMEM;

	error = sysfs_create_files(iomap_kobj, fuse_iomap_attrs);
	if (error) {
		kobject_put(iomap_kobj);
		return error;
	}

	return 0;
}

void fuse_iomap_sysfs_cleanup(struct kobject *fuse_kobj)
{
	kobject_put(iomap_kobj);
}
#endif /* IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG) */

bool fuse_iomap_enabled(void)
{
	/* Don't let anyone touch iomap until the end of the patchset. */
	return false;

	/*
	 * There are fears that a fuse+iomap server could somehow DoS the
	 * system by doing things like going out to lunch during a writeback
	 * related iomap request.  Only allow iomap access if the fuse server
	 * has rawio capabilities since those processes can mess things up
	 * quite well even without our help.
	 */
	return enable_iomap && has_capability_noaudit(current, CAP_SYS_RAWIO);
}

/* Convert IOMAP_* mapping types to FUSE_IOMAP_TYPE_* */
#define XMAP(word) \
	case IOMAP_##word: \
		return FUSE_IOMAP_TYPE_##word
static inline uint16_t fuse_iomap_type_to_server(uint16_t iomap_type)
{
	switch (iomap_type) {
	XMAP(HOLE);
	XMAP(DELALLOC);
	XMAP(MAPPED);
	XMAP(UNWRITTEN);
	XMAP(INLINE);
	default:
		ASSERT(0);
	}
	return 0;
}
#undef XMAP

/* Convert FUSE_IOMAP_TYPE_* to IOMAP_* mapping types */
#define XMAP(word) \
	case FUSE_IOMAP_TYPE_##word: \
		return IOMAP_##word
static inline uint16_t fuse_iomap_type_from_server(uint16_t fuse_type)
{
	switch (fuse_type) {
	XMAP(HOLE);
	XMAP(DELALLOC);
	XMAP(MAPPED);
	XMAP(UNWRITTEN);
	XMAP(INLINE);
	default:
		ASSERT(0);
	}
	return 0;
}
#undef XMAP

/* Validate FUSE_IOMAP_TYPE_* */
static inline bool fuse_iomap_check_type(uint16_t fuse_type)
{
	switch (fuse_type) {
	case FUSE_IOMAP_TYPE_HOLE:
	case FUSE_IOMAP_TYPE_DELALLOC:
	case FUSE_IOMAP_TYPE_MAPPED:
	case FUSE_IOMAP_TYPE_UNWRITTEN:
	case FUSE_IOMAP_TYPE_INLINE:
	case FUSE_IOMAP_TYPE_PURE_OVERWRITE:
		return true;
	}

	return false;
}

#define FUSE_IOMAP_F_ALL (FUSE_IOMAP_F_NEW | \
			  FUSE_IOMAP_F_DIRTY | \
			  FUSE_IOMAP_F_SHARED | \
			  FUSE_IOMAP_F_MERGED | \
			  FUSE_IOMAP_F_BOUNDARY | \
			  FUSE_IOMAP_F_ANON_WRITE | \
			  FUSE_IOMAP_F_ATOMIC_BIO | \
			  FUSE_IOMAP_F_WANT_IOMAP_END)

static inline bool fuse_iomap_check_flags(uint16_t flags)
{
	return (flags & ~FUSE_IOMAP_F_ALL) == 0;
}

/* Convert IOMAP_F_* mapping state flags to FUSE_IOMAP_F_* */
#define XMAP(word) \
	if (iomap_f_flags & IOMAP_F_##word) \
		ret |= FUSE_IOMAP_F_##word
#define XMAP2(iword, oword) \
	if (iomap_f_flags & IOMAP_F_##iword) \
		ret |= FUSE_IOMAP_F_##oword
static inline uint16_t fuse_iomap_flags_to_server(uint16_t iomap_f_flags)
{
	uint16_t ret = 0;

	XMAP(NEW);
	XMAP(DIRTY);
	XMAP(SHARED);
	XMAP(MERGED);
	XMAP(BOUNDARY);
	XMAP(ANON_WRITE);
	XMAP(ATOMIC_BIO);
	XMAP2(PRIVATE, WANT_IOMAP_END);

	XMAP(SIZE_CHANGED);
	XMAP(STALE);

	return ret;
}
#undef XMAP2
#undef XMAP

/* Convert FUSE_IOMAP_F_* to IOMAP_F_* mapping state flags */
#define XMAP(word) \
	if (fuse_f_flags & FUSE_IOMAP_F_##word) \
		ret |= IOMAP_F_##word
#define XMAP2(iword, oword) \
	if (fuse_f_flags & FUSE_IOMAP_F_##iword) \
		ret |= IOMAP_F_##oword
static inline uint16_t fuse_iomap_flags_from_server(uint16_t fuse_f_flags)
{
	uint16_t ret = 0;

	XMAP(NEW);
	XMAP(DIRTY);
	XMAP(SHARED);
	XMAP(MERGED);
	XMAP(BOUNDARY);
	XMAP(ANON_WRITE);
	XMAP(ATOMIC_BIO);
	XMAP2(WANT_IOMAP_END, PRIVATE);

	return ret;
}
#undef XMAP2
#undef XMAP

/* Convert IOMAP_* operation flags to FUSE_IOMAP_OP_* */
#define XMAP(word) \
	if (iomap_op_flags & IOMAP_##word) \
		ret |= FUSE_IOMAP_OP_##word
static inline uint32_t fuse_iomap_op_to_server(unsigned iomap_op_flags)
{
	uint32_t ret = 0;

	XMAP(WRITE);
	XMAP(ZERO);
	XMAP(REPORT);
	XMAP(FAULT);
	XMAP(DIRECT);
	XMAP(NOWAIT);
	XMAP(OVERWRITE_ONLY);
	XMAP(UNSHARE);
	XMAP(DAX);
	XMAP(ATOMIC);
	XMAP(DONTCACHE);

	return ret;
}
#undef XMAP

/* Validate an iomap mapping. */
static inline bool fuse_iomap_check_mapping(const struct inode *inode,
					    const struct fuse_iomap_io *map,
					    enum fuse_iomap_iodir iodir)
{
	const unsigned int blocksize = i_blocksize(inode);
	uint64_t end;

	/* Type and flags must be known */
	if (BAD_DATA(!fuse_iomap_check_type(map->type)))
		return false;
	if (BAD_DATA(!fuse_iomap_check_flags(map->flags)))
		return false;

	/* No zero-length mappings */
	if (BAD_DATA(map->length == 0))
		return false;

	/* File range must be aligned to blocksize */
	if (BAD_DATA(!IS_ALIGNED(map->offset, blocksize)))
		return false;
	if (BAD_DATA(!IS_ALIGNED(map->length, blocksize)))
		return false;

	/* No overflows in the file range */
	if (BAD_DATA(check_add_overflow(map->offset, map->length, &end)))
		return false;

	/* File range cannot start past maxbytes */
	if (BAD_DATA(map->offset >= inode->i_sb->s_maxbytes))
		return false;

	switch (map->type) {
	case FUSE_IOMAP_TYPE_MAPPED:
	case FUSE_IOMAP_TYPE_UNWRITTEN:
		/* Mappings backed by space must have a device/addr */
		if (BAD_DATA(map->dev == FUSE_IOMAP_DEV_NULL))
			return false;
		if (BAD_DATA(map->addr == FUSE_IOMAP_NULL_ADDR))
			return false;
		break;
	case FUSE_IOMAP_TYPE_DELALLOC:
	case FUSE_IOMAP_TYPE_HOLE:
	case FUSE_IOMAP_TYPE_INLINE:
		/* Mappings not backed by space cannot have a device addr. */
		if (BAD_DATA(map->dev != FUSE_IOMAP_DEV_NULL))
			return false;
		if (BAD_DATA(map->addr != FUSE_IOMAP_NULL_ADDR))
			return false;
		break;
	case FUSE_IOMAP_TYPE_PURE_OVERWRITE:
		/* "Pure overwrite" only allowed for write mapping */
		if (BAD_DATA(iodir != WRITE_MAPPING))
			return false;
		break;
	default:
		/* should have been caught already */
		ASSERT(0);
		return false;
	}

	/* No overflows in the device range, if supplied */
	if (map->addr != FUSE_IOMAP_NULL_ADDR &&
	    BAD_DATA(check_add_overflow(map->addr, map->length, &end)))
		return false;

	return true;
}

/* Convert a mapping from the server into something the kernel can use */
static inline void fuse_iomap_from_server(struct iomap *iomap,
					  const struct fuse_backing *fb,
					  const struct fuse_iomap_io *fmap)
{
	iomap->addr = fmap->addr;
	iomap->offset = fmap->offset;
	iomap->length = fmap->length;
	iomap->type = fuse_iomap_type_from_server(fmap->type);
	iomap->flags = fuse_iomap_flags_from_server(fmap->flags);
	iomap->bdev = fb ? fb->bdev : NULL;
	iomap->dax_dev = NULL;
}

static bool fuse_iomap_matches_bdev(const struct fuse_backing *fb,
				    const void *data)
{
	return fb->bdev == data;
}

static inline uint32_t
fuse_iomap_find_backing_id(struct fuse_conn *fc,
			   const struct block_device *bdev)
{
	int ret = -ENODEV;

	if (bdev)
		ret = fuse_backing_lookup_id(fc, fuse_iomap_matches_bdev, bdev);
	if (ret < 0)
		return FUSE_IOMAP_DEV_NULL;
	return ret;
}

/* Convert a mapping from the kernel into something the server can use */
static inline void fuse_iomap_to_server(struct fuse_conn *fc,
					struct fuse_iomap_io *fmap,
					const struct iomap *iomap)
{
	fmap->addr = fmap->addr;
	fmap->offset = iomap->offset;
	fmap->length = iomap->length;
	fmap->type = fuse_iomap_type_to_server(iomap->type);
	fmap->flags = fuse_iomap_flags_to_server(iomap->flags);
	fmap->dev = fuse_iomap_find_backing_id(fc, iomap->bdev);
}

/* Check the incoming _begin mappings to make sure they're not nonsense. */
static inline int
fuse_iomap_begin_validate(const struct inode *inode,
			  unsigned opflags, loff_t pos,
			  const struct fuse_iomap_begin_out *outarg)
{
	/* Make sure the mappings aren't garbage */
	if (!fuse_iomap_check_mapping(inode, &outarg->read, READ_MAPPING))
		return -EFSCORRUPTED;

	if (!fuse_iomap_check_mapping(inode, &outarg->write, WRITE_MAPPING))
		return -EFSCORRUPTED;

	/*
	 * Must have returned a mapping for at least the first byte in the
	 * range.  The main mapping check already validated that the length
	 * is nonzero and there is no overflow in computing end.
	 */
	if (BAD_DATA(outarg->read.offset > pos))
		return -EFSCORRUPTED;
	if (BAD_DATA(outarg->write.offset > pos))
		return -EFSCORRUPTED;

	if (BAD_DATA(outarg->read.offset + outarg->read.length <= pos))
		return -EFSCORRUPTED;
	if (BAD_DATA(outarg->write.offset + outarg->write.length <= pos))
		return -EFSCORRUPTED;

	return 0;
}

static inline bool fuse_is_iomap_file_write(unsigned int opflags)
{
	return opflags & (IOMAP_WRITE | IOMAP_ZERO | IOMAP_UNSHARE);
}

static inline struct fuse_backing *
fuse_iomap_find_dev(struct fuse_conn *fc, const struct fuse_iomap_io *map)
{
	struct fuse_backing *ret = NULL;

	if (map->dev != FUSE_IOMAP_DEV_NULL && map->dev < INT_MAX)
		ret = fuse_backing_lookup(fc, &fuse_iomap_backing_ops,
					  map->dev);

	switch (map->type) {
	case FUSE_IOMAP_TYPE_MAPPED:
	case FUSE_IOMAP_TYPE_UNWRITTEN:
		/* Mappings backed by space must have a device/addr */
		if (BAD_DATA(ret == NULL))
			return ERR_PTR(-EFSCORRUPTED);
		break;
	}

	return ret;
}

static int fuse_iomap_begin(struct inode *inode, loff_t pos, loff_t count,
			    unsigned opflags, struct iomap *iomap,
			    struct iomap *srcmap)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_begin_in inarg = {
		.attr_ino = fi->orig_ino,
		.opflags = fuse_iomap_op_to_server(opflags),
		.pos = pos,
		.count = count,
	};
	struct fuse_iomap_begin_out outarg = { };
	struct fuse_mount *fm = get_fuse_mount(inode);
	struct fuse_backing *read_dev = NULL;
	struct fuse_backing *write_dev = NULL;
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

	trace_fuse_iomap_read_map(inode, &outarg.read);
	trace_fuse_iomap_write_map(inode, &outarg.write);

	err = fuse_iomap_begin_validate(inode, opflags, pos, &outarg);
	if (err)
		return err;

	read_dev = fuse_iomap_find_dev(fm->fc, &outarg.read);
	if (IS_ERR(read_dev))
		return PTR_ERR(read_dev);

	if (fuse_is_iomap_file_write(opflags) &&
	    outarg.write.type != FUSE_IOMAP_TYPE_PURE_OVERWRITE) {
		/* open the write device */
		write_dev = fuse_iomap_find_dev(fm->fc, &outarg.write);
		if (IS_ERR(write_dev)) {
			err = PTR_ERR(write_dev);
			goto out_read_dev;
		}

		/*
		 * For an out of place write, we must supply the write mapping
		 * via @iomap, and the read mapping via @srcmap.
		 */
		fuse_iomap_from_server(iomap, write_dev, &outarg.write);
		fuse_iomap_from_server(srcmap, read_dev, &outarg.read);
	} else {
		/*
		 * For everything else (reads, reporting, and pure overwrites),
		 * we can return the sole mapping through @iomap and leave
		 * @srcmap unchanged from its default (HOLE).
		 */
		fuse_iomap_from_server(iomap, read_dev, &outarg.read);
	}

	/*
	 * XXX: if we ever want to support closing devices, we need a way to
	 * track the fuse_backing refcount all the way through bio endios.
	 * For now we put the refcount here because you can't remove an iomap
	 * device until unmount time.
	 */
	fuse_backing_put(write_dev);
out_read_dev:
	fuse_backing_put(read_dev);
	return err;
}

/* Decide if we send FUSE_IOMAP_END to the fuse server */
static bool fuse_should_send_iomap_end(const struct iomap *iomap,
				       unsigned int opflags, loff_t count,
				       ssize_t written)
{
	/* fuse server demanded an iomap_end call. */
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
	struct fuse_mount *fm = get_fuse_mount(inode);
	int err;

	if (fuse_should_send_iomap_end(iomap, opflags, count, written)) {
		struct fuse_iomap_end_in inarg = {
			.opflags = fuse_iomap_op_to_server(opflags),
			.attr_ino = fi->orig_ino,
			.pos = pos,
			.count = count,
			.written = written,
		};
		FUSE_ARGS(args);

		fuse_iomap_to_server(fm->fc, &inarg.map, iomap);

		trace_fuse_iomap_end(inode, &inarg);

		args.opcode = FUSE_IOMAP_END;
		args.nodeid = get_node_id(inode);
		args.in_numargs = 1;
		args.in_args[0].size = sizeof(inarg);
		args.in_args[0].value = &inarg;
		err = fuse_simple_request(fm, &args);
		if (err == -ENOSYS) {
			/*
			 * libfuse returns ENOSYS for servers that don't
			 * implement iomap_end
			 */
			err = 0;
		}
		if (err) {
			trace_fuse_iomap_end_error(inode, &inarg, err);
			return err;
		}
	}

	return 0;
}

const struct iomap_ops fuse_iomap_ops = {
	.iomap_begin		= fuse_iomap_begin,
	.iomap_end		= fuse_iomap_end,
};

static int fuse_iomap_may_admin(struct fuse_conn *fc, unsigned int flags)
{
	if (!fc->iomap)
		return -EPERM;

	if (flags)
		return -EINVAL;

	return 0;
}

static int fuse_iomap_may_open(struct fuse_conn *fc, struct file *file)
{
	if (!S_ISBLK(file_inode(file)->i_mode))
		return -ENODEV;

	return 0;
}

static int fuse_iomap_post_open(struct fuse_conn *fc, struct fuse_backing *fb)
{
	fb->bdev = I_BDEV(fb->file->f_mapping->host);
	return 0;
}

static int fuse_iomap_may_close(struct fuse_conn *fc, struct file *file)
{
	/* We only support closing iomap block devices at unmount */
	return -EBUSY;
}

const struct fuse_backing_ops fuse_iomap_backing_ops = {
	.type = FUSE_BACKING_TYPE_IOMAP,
	.id_start = 1,
	.id_end = 1025,		/* maximum 1024 block devices */
	.may_admin = fuse_iomap_may_admin,
	.may_open = fuse_iomap_may_open,
	.may_close = fuse_iomap_may_close,
	.post_open = fuse_iomap_post_open,
};
