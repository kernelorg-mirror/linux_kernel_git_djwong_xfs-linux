// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <linux/iomap.h>
#include <linux/fiemap.h>
#include <linux/pagemap.h>
#include <linux/falloc.h>
#include "fuse_i.h"
#include "fuse_trace.h"
#include "fuse_iomap.h"
#include "fuse_iomap_i.h"

static bool __read_mostly enable_iomap =
#if IS_ENABLED(CONFIG_FUSE_IOMAP_BY_DEFAULT)
	true;
#else
	false;
#endif
module_param(enable_iomap, bool, 0644);
MODULE_PARM_DESC(enable_iomap, "Enable file I/O through iomap");

#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG)
#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG_BY_DEFAULT)
DEFINE_STATIC_KEY_TRUE(fuse_iomap_debug);
#else
DEFINE_STATIC_KEY_FALSE(fuse_iomap_debug);
#endif /* FUSE_IOMAP_DEBUG_BY_DEFAULT */

static int iomap_debug_set(const char *val, const struct kernel_param *kp)
{
	bool now;
	int ret;

	if (!val)
		return -EINVAL;

	ret = kstrtobool(val, &now);
	if (ret)
		return ret;

	if (now)
		static_branch_enable(&fuse_iomap_debug);
	else
		static_branch_disable(&fuse_iomap_debug);

	return 0;
}

static int iomap_debug_get(char *buffer, const struct kernel_param *kp)
{
	return sprintf(buffer, "%c\n",
		       static_branch_unlikely(&fuse_iomap_debug) ? 'Y' : 'N');
}

static const struct kernel_param_ops iomap_debug_ops = {
	.set = iomap_debug_set,
	.get = iomap_debug_get,
};

module_param_cb(debug_iomap, &iomap_debug_ops, NULL, 0644);
__MODULE_PARM_TYPE(debug_iomap, "bool");
MODULE_PARM_DESC(debug_iomap, "Enable debugging of fuse iomap");
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
	uint32_t ret = iomap_op_flags & FUSE_IOMAP_OP_WRITEBACK;

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
	int ret;

	if (!bdev)
		return FUSE_IOMAP_DEV_NULL;
	ret = fuse_backing_lookup_id(fc, &fuse_iomap_backing_ops,
				     fuse_iomap_matches_bdev, bdev);
	if (ret <= 0)
		return FUSE_IOMAP_DEV_NULL;
	return ret;
}

/* Convert a mapping from the kernel into something the server can use */
static inline void fuse_iomap_to_server(struct fuse_conn *fc,
					struct fuse_iomap_io *fmap,
					const struct iomap *iomap)
{
	fmap->addr = iomap->addr;
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
	return opflags & (IOMAP_WRITE | IOMAP_ZERO | IOMAP_UNSHARE |
			  FUSE_IOMAP_OP_WRITEBACK);
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
	 * It's ok to put the refcount here because you can't remove an iomap
	 * device unless there are zero iomap inodes.
	 */
	fuse_backing_put(write_dev);
out_read_dev:
	fuse_backing_put(read_dev);
	return err;
}

/* Decide if we send FUSE_IOMAP_END to the fuse server */
static bool fuse_should_send_iomap_end(const struct fuse_mount *fm,
				       const struct iomap *iomap,
				       unsigned int opflags, loff_t count,
				       ssize_t written)
{
	/* Not implemented on fuse server */
	if (fm->fc->iomap_conn.no_end)
		return false;

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

	if (fuse_should_send_iomap_end(fm, iomap, opflags, count, written)) {
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
			fm->fc->iomap_conn.no_end = 1;
			err = 0;
		}
		if (err) {
			trace_fuse_iomap_end_error(inode, &inarg, err);
			return err;
		}
	}

	return 0;
}

static const struct iomap_ops fuse_iomap_ops = {
	.iomap_begin		= fuse_iomap_begin,
	.iomap_end		= fuse_iomap_end,
};

static inline bool
fuse_should_send_iomap_ioend(const struct fuse_mount *fm,
			     const struct fuse_iomap_ioend_in *inarg)
{
	/* Not implemented on fuse server */
	if (fm->fc->iomap_conn.no_ioend)
		return false;

	/* Always send an ioend for errors. */
	if (inarg->error)
		return true;

	/* Send an ioend if we performed an IO involving metadata changes. */
	return inarg->written > 0 &&
	       (inarg->flags & (FUSE_IOMAP_IOEND_SHARED |
				FUSE_IOMAP_IOEND_UNWRITTEN |
				FUSE_IOMAP_IOEND_APPEND));
}

int
fuse_iomap_setsize_finish(
	struct inode		*inode,
	loff_t			newsize)
{
	struct fuse_inode *fi = get_fuse_inode(inode);

	ASSERT(fuse_inode_has_iomap(inode));

	trace_fuse_iomap_setsize_finish(inode, newsize, 0);

	spin_lock(&fi->lock);
	fi->i_disk_size = newsize;
	spin_unlock(&fi->lock);
	return 0;
}

static int fuse_iomap_ioend(struct inode *inode, loff_t pos, size_t written,
			    int error, unsigned ioendflags,
			    struct block_device *bdev, sector_t new_addr)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_mount *fm = get_fuse_mount(inode);
	struct fuse_iomap_ioend_in inarg = {
		.flags = ioendflags,
		.error = error,
		.attr_ino = fi->orig_ino,
		.pos = pos,
		.written = written,
		.dev = fuse_iomap_find_backing_id(fm->fc, bdev),
		.new_addr = new_addr,
	};
	struct fuse_iomap_ioend_out outarg = { };

	spin_lock(&fi->lock);
	if (pos + written > fi->i_disk_size)
		inarg.flags |= FUSE_IOMAP_IOEND_APPEND;

	outarg.newsize = max_t(loff_t, fi->i_disk_size, pos + written),
	spin_unlock(&fi->lock);

	trace_fuse_iomap_ioend(inode, &inarg);

	if (fuse_should_send_iomap_ioend(fm, &inarg)) {
		FUSE_ARGS(args);
		int iomap_error;

		args.opcode = FUSE_IOMAP_IOEND;
		args.nodeid = get_node_id(inode);
		args.in_numargs = 1;
		args.in_args[0].size = sizeof(inarg);
		args.in_args[0].value = &inarg;
		args.out_numargs = 1;
		args.out_args[0].size = sizeof(outarg);
		args.out_args[0].value = &outarg;
		iomap_error = fuse_simple_request(fm, &args);
		switch (iomap_error) {
		case -ENOSYS:
			/*
			 * fuse servers can return ENOSYS if ioend processing
			 * is never needed for this filesystem.  Don't pass
			 * that up to iomap.
			 */
			fm->fc->iomap_conn.no_ioend = 1;
			break;
		case 0:
			break;
		default:
			trace_fuse_iomap_ioend_error(inode, &inarg, &outarg,
						     iomap_error);

			/*
			 * If the write IO failed, return the failure code to
			 * the caller no matter what happens with the ioend.
			 * If the write IO succeeded but the ioend did not,
			 * pass the new error up to the caller.
			 */
			if (!error)
				error = iomap_error;
			break;
		}
	}

	/*
	 * Pass whatever error iomap gave us (or any new errors since then)
	 * back to iomap.
	 */
	if (error)
		return error;

	/*
	 * If there weren't any ioend errors, update the incore isize, which
	 * confusingly takes the new i_size as "pos".
	 */
	spin_lock(&fi->lock);
	fi->i_disk_size = outarg.newsize;
	spin_unlock(&fi->lock);
	fuse_write_update_attr(inode, pos + written, written);
	return 0;
}

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

static int fuse_iomap_may_close(struct fuse_conn *fc,
				const struct fuse_backing *fb)
{
	/*
	 * It's only safe to close the iomap backing device if there are no
	 * iomap inodes that might be using it or have IO in progress.
	 */
	if (atomic64_read(&fc->iomap_conn.inodes) > 0)
		return -EBUSY;
	return 0;
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

void fuse_iomap_mount(struct fuse_mount *fm)
{
	struct fuse_conn *fc = fm->fc;
	struct super_block *sb = fm->sb;
	struct backing_dev_info *old_bdi = sb->s_bdi;
	char *suffix = sb->s_bdev ? "-fuseblk" : "-fuse";
	int res;

	/*
	 * sb->s_bdi points to the initial private bdi.  However, we want to
	 * redirect it to a new private bdi with default dirty and readahead
	 * settings because iomap writeback won't be pushing a ton of dirty
	 * data through the fuse device.  If this fails we fall back to the
	 * initial fuse bdi.
	 */
	sb->s_bdi = &noop_backing_dev_info;
	res = super_setup_bdi_name(sb, "%u:%u%s.iomap", MAJOR(fc->dev),
				   MINOR(fc->dev), suffix);
	if (res) {
		sb->s_bdi = old_bdi;
	} else {
		bdi_unregister(old_bdi);
		bdi_put(old_bdi);
	}

	/*
	 * Enable syncfs for iomap fuse servers so that we can send a final
	 * flush at unmount time.  This also means that we can support
	 * freeze/thaw properly.
	 */
	fc->sync_fs = true;
}

void fuse_iomap_unmount(struct fuse_mount *fm)
{
	struct fuse_conn *fc = fm->fc;

	/*
	 * Flush all pending file release commands and send a destroy command.
	 * This gives the fuse server a chance to process all the pending
	 * releases, write the last bits of metadata changes to disk, and close
	 * the iomap block devices before we return from the umount call.
	 * iomap fuse servers are expected to release all exclusive access
	 * resources before unmount completes.
	 *
	 * Note that multithreaded fuse servers will have to hold the destroy
	 * command until all release requests have completed because the kernel
	 * maintainers do not want to introduce waits in unmount.
	 */
	fuse_flush_requests(fc);
	fuse_send_destroy(fm);
}

static inline void fuse_inode_set_iomap(struct inode *inode);

static inline void fuse_inode_clear_iomap(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_mount *fm = get_fuse_mount(inode);

	atomic64_dec(&fm->fc->iomap_conn.inodes);
	clear_bit(FUSE_I_IOMAP, &fi->state);
}

void fuse_iomap_init_inode(struct inode *inode, struct fuse_attr *attr)
{
	ASSERT(get_fuse_conn(inode)->iomap);

	if (!(attr->flags & FUSE_ATTR_IOMAP))
		return;

	/*
	 * Any file being used in conjunction with iomap must also have the
	 * exclusive flag set because iomap requires cached file attributes to
	 * be correct at any time.  This applies even to non-regular files
	 * (e.g. directories) because we need to do ACL and attribute
	 * inheritance the same way a local filesystem would do.  If exclusive
	 * mode isn't set, then we won't use iomap.
	 */
	if (!fuse_inode_is_exclusive(inode)) {
		ASSERT(fuse_inode_is_exclusive(inode));
		return;
	}

	if (!S_ISREG(inode->i_mode)) {
		trace_fuse_iomap_init_inode(inode);
		return;
	}

	fuse_inode_set_iomap(inode);

	trace_fuse_iomap_init_inode(inode);
}

void fuse_iomap_evict_inode(struct inode *inode)
{
	ASSERT(fuse_inode_has_iomap(inode));

	trace_fuse_iomap_evict_inode(inode);

	fuse_inode_clear_iomap(inode);
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
	if (!fuse_inode_has_iomap(inode))
		return -EOPNOTSUPP;

	if (fieinfo->fi_flags & FIEMAP_FLAG_XATTR)
		return -EOPNOTSUPP;

	if (fuse_is_bad(inode))
		return -EIO;

	if (!fuse_allow_current_process(fc))
		return -EACCES;

	trace_fuse_iomap_fiemap(inode, start, count, fieinfo->fi_flags);

	inode_lock_shared(inode);
	error = iomap_fiemap(inode, fieinfo, start, count, &fuse_iomap_ops);
	inode_unlock_shared(inode);

	return error;
}

sector_t fuse_iomap_bmap(struct address_space *mapping, sector_t block)
{
	ASSERT(fuse_inode_has_iomap(mapping->host));

	return iomap_bmap(mapping, block, &fuse_iomap_ops);
}

loff_t fuse_iomap_lseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	struct fuse_conn *fc = get_fuse_conn(inode);

	ASSERT(fuse_inode_has_iomap(inode));

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
		return generic_file_llseek(file, offset, whence);
	}

	if (offset < 0)
		return offset;
	return vfs_setpos(file, offset, inode->i_sb->s_maxbytes);
}

void fuse_iomap_open(struct inode *inode, struct file *file)
{
	ASSERT(fuse_inode_has_iomap(inode));

	file->f_mode |= FMODE_NOWAIT | FMODE_CAN_ODIRECT;
}

int fuse_iomap_finish_open(const struct fuse_file *ff,
			   const struct inode *inode)
{
	ASSERT(fuse_inode_has_iomap(inode));

	/* no weird modes, iomap only handles seekable regular files */
	if (ff->open_flags & (FOPEN_PASSTHROUGH |
			      FOPEN_STREAM |
			      FOPEN_NONSEEKABLE))
		return -EINVAL;

	return 0;
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

		/* shut up gcc */
		return 0;
	}

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

	return 0;
}

static ssize_t fuse_iomap_direct_read(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	trace_fuse_iomap_direct_read(iocb, to);

	if (!iov_iter_count(to))
		return 0; /* skip atime */

	ret = fuse_iomap_ilock_iocb(iocb, SHARED);
	if (ret)
		return ret;
	ret = iomap_dio_rw(iocb, to, &fuse_iomap_ops, NULL, 0, NULL, 0);
	if (ret > 0)
		file_accessed(iocb->ki_filp);
	inode_unlock_shared(inode);

	trace_fuse_iomap_direct_read_end(iocb, to, ret);
	return ret;
}

static int fuse_iomap_dio_write_end_io(struct kiocb *iocb, ssize_t written,
				       int error, unsigned dioflags)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	unsigned int ioendflags = FUSE_IOMAP_IOEND_DIRECT;

	if (fuse_is_bad(inode))
		return -EIO;

	ASSERT(fuse_inode_has_iomap(inode));

	trace_fuse_iomap_dio_write_end_io(inode, iocb->ki_pos, written, error,
					  dioflags);

	if (dioflags & IOMAP_DIO_COW)
		ioendflags |= FUSE_IOMAP_IOEND_SHARED;
	if (dioflags & IOMAP_DIO_UNWRITTEN)
		ioendflags |= FUSE_IOMAP_IOEND_UNWRITTEN;

	return fuse_iomap_ioend(inode, iocb->ki_pos, written, error,
				ioendflags, NULL, FUSE_IOMAP_NULL_ADDR);
}

static const struct iomap_dio_ops fuse_iomap_dio_write_ops = {
	.end_io		= fuse_iomap_dio_write_end_io,
};

static const struct iomap_write_ops fuse_iomap_write_ops = {
};

static int
fuse_iomap_zero_range(
	struct inode		*inode,
	loff_t			pos,
	loff_t			len,
	bool			*did_zero)
{
	return iomap_zero_range(inode, pos, len, did_zero, &fuse_iomap_ops,
				&fuse_iomap_write_ops, NULL);
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
	if (fuse_inode_has_iomap(inode) &&
	    iocb->ki_pos > i_size_read(inode)) {
		error = fuse_iomap_write_zero_eof(iocb, from, &drained_dio);
		if (error == 1)
			goto restart;
		if (error)
			return error;
	}

	return kiocb_modified(iocb);
}

static ssize_t fuse_iomap_direct_write(struct kiocb *iocb,
				       struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t blockmask = i_blocksize(inode) - 1;
	size_t count = iov_iter_count(from);
	unsigned int flags = IOMAP_DIO_COMP_WORK;
	ssize_t ret;

	trace_fuse_iomap_direct_write(iocb, from);

	if (!count)
		return 0;

	/*
	 * Unaligned direct writes require zeroing of unwritten head and tail
	 * blocks.  Extending writes require zeroing of post-EOF tail blocks.
	 * The zeroing writes must complete before we return the direct write
	 * to userspace.  Don't even bother trying the fast path.
	 */
	if ((iocb->ki_pos | count) & blockmask)
		flags |= IOMAP_DIO_FORCE_WAIT;

	ret = fuse_iomap_ilock_iocb(iocb, EXCL);
	if (ret)
		goto out_dsync;

	ret = fuse_iomap_write_checks(iocb, from);
	if (ret)
		goto out_unlock;

	/*
	 * If we are doing exclusive unaligned I/O, this must be the only I/O
	 * in-flight.  Otherwise we risk data corruption due to unwritten
	 * extent conversions from the AIO end_io handler.  Wait for all other
	 * I/O to drain first.
	 */
	if (flags & IOMAP_DIO_FORCE_WAIT)
		inode_dio_wait(inode);

	ret = iomap_dio_rw(iocb, from, &fuse_iomap_ops,
			   &fuse_iomap_dio_write_ops, flags, NULL, 0);
out_unlock:
	inode_unlock(inode);
out_dsync:
	trace_fuse_iomap_direct_write_end(iocb, from, ret);
	return ret;
}

void fuse_iomap_set_disk_size(struct fuse_inode *fi, loff_t newsize)
{
	lockdep_assert_held(&fi->lock);

	if (fuse_inode_has_iomap(&fi->inode))
		fi->i_disk_size = newsize;
}

void fuse_iomap_open_truncate(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);

	ASSERT(fuse_inode_has_iomap(inode));

	trace_fuse_iomap_open_truncate(inode);

	spin_lock(&fi->lock);
	fi->i_disk_size = 0;
	spin_unlock(&fi->lock);
}

struct fuse_writepage_ctx {
	struct iomap_writepage_ctx ctx;
};

static void fuse_iomap_end_ioend(struct iomap_ioend *ioend)
{
	struct inode *inode = ioend->io_inode;
	unsigned int ioendflags = FUSE_IOMAP_IOEND_WRITEBACK;
	unsigned int nofs_flag;
	int error = blk_status_to_errno(ioend->io_bio.bi_status);

	ASSERT(fuse_inode_has_iomap(inode));

	/* We still have to clean up the ioend even if the inode is dead */
	if (!error && fuse_is_bad(inode))
		error = -EIO;

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
			 ioendflags, ioend->io_bio.bi_bdev, ioend->io_sector);
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

	ASSERT(fuse_inode_has_iomap(inode));

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
static void fuse_iomap_discard_folio(struct folio *folio, loff_t pos, int error)
{
	struct inode *inode = folio->mapping->host;
	struct fuse_inode *fi = get_fuse_inode(inode);
	loff_t end = folio_pos(folio) + folio_size(folio);

	if (fuse_is_bad(inode))
		return;

	ASSERT(fuse_inode_has_iomap(inode));

	trace_fuse_iomap_discard_folio(inode, pos, folio_size(folio));

	printk_ratelimited(KERN_ERR
		"page discard on page %px, inode 0x%llx, pos %llu.",
			folio, fi->orig_ino, pos);

	/* Userspace may need to remove delayed allocations */
	fuse_iomap_ioend(inode, pos, end - pos, error, 0, NULL,
			 FUSE_IOMAP_NULL_ADDR);
}

static ssize_t fuse_iomap_writeback_range(struct iomap_writepage_ctx *wpc,
					  struct folio *folio, u64 offset,
					  unsigned int len, u64 end_pos)
{
	struct inode *inode = wpc->inode;
	struct iomap write_iomap, dontcare;
	ssize_t ret;

	if (fuse_is_bad(inode)) {
		ret = -EIO;
		goto discard_folio;
	}

	ASSERT(fuse_inode_has_iomap(inode));

	trace_fuse_iomap_writeback_range(inode, offset, len, end_pos);

	if (!fuse_iomap_revalidate_writeback(wpc, offset)) {
		ret = fuse_iomap_begin(inode, offset, len,
				       FUSE_IOMAP_OP_WRITEBACK,
				       &write_iomap, &dontcare);
		if (ret)
			goto discard_folio;

		/*
		 * Landed in a hole or beyond EOF?  Send that to iomap, it'll
		 * skip writing back the file range.
		 */
		if (write_iomap.offset > offset) {
			write_iomap.length = write_iomap.offset - offset;
			write_iomap.offset = offset;
			write_iomap.type = IOMAP_HOLE;
		}

		memcpy(&wpc->iomap, &write_iomap, sizeof(struct iomap));
	}

	ret = iomap_add_to_ioend(wpc, folio, offset, end_pos, len);
	if (ret < 0)
		goto discard_folio;

	return ret;
discard_folio:
	fuse_iomap_discard_folio(folio, offset, ret);
	return ret;
}

static int fuse_iomap_writeback_submit(struct iomap_writepage_ctx *wpc,
				       int error)
{
	struct iomap_ioend *ioend = wpc->wb_ctx;

	ASSERT(fuse_inode_has_iomap(ioend->io_inode));

	trace_fuse_iomap_writeback_submit(wpc, error);

	/* always call our ioend function, even if we cancel the bio */
	ioend->io_bio.bi_end_io = fuse_iomap_end_bio;
	return iomap_ioend_writeback_submit(wpc, error);
}

static const struct iomap_writeback_ops fuse_iomap_writeback_ops = {
	.writeback_range	= fuse_iomap_writeback_range,
	.writeback_submit	= fuse_iomap_writeback_submit,
};

static int fuse_iomap_writepages(struct address_space *mapping,
				 struct writeback_control *wbc)
{
	struct fuse_writepage_ctx wpc = {
		.ctx = {
			.inode = mapping->host,
			.wbc = wbc,
			.ops = &fuse_iomap_writeback_ops,
		},
	};

	ASSERT(fuse_inode_has_iomap(mapping->host));

	trace_fuse_iomap_writepages(mapping->host, wbc);

	return iomap_writepages(&wpc.ctx);
}

static int fuse_iomap_read_folio(struct file *file, struct folio *folio)
{
	ASSERT(fuse_inode_has_iomap(file_inode(file)));

	trace_fuse_iomap_read_folio(folio);

	iomap_bio_read_folio(folio, &fuse_iomap_ops);
	return 0;
}

static void fuse_iomap_readahead(struct readahead_control *rac)
{
	ASSERT(fuse_inode_has_iomap(file_inode(rac->file)));

	trace_fuse_iomap_readahead(rac);

	iomap_bio_readahead(rac, &fuse_iomap_ops);
}

static const struct address_space_operations fuse_iomap_aops = {
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
};

static inline void fuse_inode_set_iomap(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_mount *fm = get_fuse_mount(inode);
	unsigned int min_order = 0;

	inode->i_data.a_ops = &fuse_iomap_aops;

	INIT_WORK(&fi->ioend_work, fuse_iomap_end_io);
	INIT_LIST_HEAD(&fi->ioend_list);
	spin_lock_init(&fi->ioend_lock);

	if (inode->i_blkbits > PAGE_SHIFT)
		min_order = inode->i_blkbits - PAGE_SHIFT;

	mapping_set_folio_min_order(inode->i_mapping, min_order);
	set_bit(FUSE_I_IOMAP, &fi->state);
	atomic64_inc(&fm->fc->iomap_conn.inodes);
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

	ASSERT(fuse_inode_has_iomap(inode));

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
	ASSERT(fuse_inode_has_iomap(file_inode(file)));

	file_accessed(file);
	vma->vm_ops = &fuse_iomap_vm_ops;
	return 0;
}

static ssize_t fuse_iomap_buffered_read(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	ASSERT(fuse_inode_has_iomap(inode));

	trace_fuse_iomap_buffered_read(iocb, to);

	if (!iov_iter_count(to))
		return 0; /* skip atime */

	ret = fuse_iomap_ilock_iocb(iocb, SHARED);
	if (ret)
		return ret;
	ret = generic_file_read_iter(iocb, to);
	if (ret > 0)
		file_accessed(iocb->ki_filp);
	inode_unlock_shared(inode);

	trace_fuse_iomap_buffered_read_end(iocb, to, ret);
	return ret;
}

static ssize_t fuse_iomap_buffered_write(struct kiocb *iocb,
					 struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct fuse_inode *fi = get_fuse_inode(inode);
	loff_t pos = iocb->ki_pos;
	ssize_t ret;

	ASSERT(fuse_inode_has_iomap(inode));

	trace_fuse_iomap_buffered_write(iocb, from);

	if (!iov_iter_count(from))
		return 0;

	ret = fuse_iomap_ilock_iocb(iocb, EXCL);
	if (ret)
		return ret;

	ret = fuse_iomap_write_checks(iocb, from);
	if (ret)
		goto out_unlock;

	if (inode->i_size < pos + iov_iter_count(from))
		set_bit(FUSE_I_SIZE_UNSTABLE, &fi->state);

	ret = iomap_file_buffered_write(iocb, from, &fuse_iomap_ops,
					&fuse_iomap_write_ops, NULL);

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

static inline bool fuse_iomap_force_directio(const struct kiocb *iocb)
{
	struct fuse_file *ff = iocb->ki_filp->private_data;

	return ff->open_flags & FOPEN_DIRECT_IO;
}

ssize_t fuse_iomap_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	const bool force_directio = fuse_iomap_force_directio(iocb);

	ASSERT(fuse_inode_has_iomap(file_inode(iocb->ki_filp)));

	if ((iocb->ki_flags & IOCB_DIRECT) || force_directio) {
		ssize_t ret = fuse_iomap_direct_read(iocb, to);

		switch (ret) {
		case -ENOTBLK:
		case -ENOSYS:
			/*
			 * We fall back to a buffered read if:
			 *
			 *  - ENOTBLK means iomap told us to do it
			 *  - ENOSYS means the fuse server wants it
			 *
			 * Don't fall back if we were forced to do it.
			 */
			if (force_directio)
				return -EIO;
			break;
		default:
			/* errors, no progress, or partial progress */
			return ret;
		}

		/* do not let generic_file_read_iter fall into ->direct_IO */
		iocb->ki_flags &= ~IOCB_DIRECT;
	}

	return fuse_iomap_buffered_read(iocb, to);
}

ssize_t fuse_iomap_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	const bool force_directio = fuse_iomap_force_directio(iocb);

	ASSERT(fuse_inode_has_iomap(file_inode(iocb->ki_filp)));

	if ((iocb->ki_flags & IOCB_DIRECT) || force_directio) {
		ssize_t ret = fuse_iomap_direct_write(iocb, from);

		switch (ret) {
		case -ENOTBLK:
		case -ENOSYS:
			/*
			 * We fall back to a buffered write if:
			 *
			 *  - ENOTBLK means iomap told us to do it
			 *  - ENOSYS means the fuse server wants it
			 *
			 * Either way, try the write again as a synchronous
			 * buffered write unless we were forced to do directio.
			 */
			if (force_directio)
				return -EIO;
			iocb->ki_flags |= IOCB_SYNC;
			break;
		default:
			/* errors, no progress, or partial progress */
			return ret;
		}
	}

	return fuse_iomap_buffered_write(iocb, from);
}

static int
fuse_iomap_truncate_page(
	struct inode *inode,
	loff_t			pos,
	bool			*did_zero)
{
	return iomap_truncate_page(inode, pos, did_zero, &fuse_iomap_ops,
				   &fuse_iomap_write_ops, NULL);
}
/*
 * Truncate pagecache for a file before sending the truncate request to
 * userspace.  Must have write permission and not be a directory.
 *
 * Caution: The caller of this function is responsible for calling
 * setattr_prepare() or otherwise verifying the change is fine.
 */
int
fuse_iomap_setsize_start(
	struct inode		*inode,
	loff_t			newsize)
{
	loff_t			oldsize = i_size_read(inode);
	int			error;
	bool			did_zeroing = false;

	rwsem_assert_held_write(&inode->i_rwsem);
	rwsem_assert_held_write(&inode->i_mapping->invalidate_lock);
	ASSERT(S_ISREG(inode->i_mode));
	ASSERT(fuse_inode_has_iomap(inode));

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
	 * Flush the entire pagecache to ensure the fuse server logs the inode
	 * size change and all dirty data that might be associated with it.
	 * We don't know the ondisk inode size, so we only have this clumsy
	 * hammer.
	 */
	return filemap_write_and_wait(inode->i_mapping);
}

/*
 * Prepare for a file data block remapping operation by flushing and unmapping
 * all pagecache for the entire range.
 */
int fuse_iomap_flush_unmap_range(struct inode *inode, loff_t pos,
				 loff_t endpos)
{
	loff_t			start, end;
	unsigned int		rounding;
	int			error;

	ASSERT(fuse_inode_has_iomap(inode));

	/*
	 * Make sure we extend the flush out to extent alignment boundaries so
	 * any extent range overlapping the start/end of the modification we
	 * are about to do is clean and idle.
	 */
	rounding = max_t(unsigned int, i_blocksize(inode), PAGE_SIZE);
	start = round_down(pos, rounding);
	end = round_up(endpos + 1, rounding) - 1;

	trace_fuse_iomap_flush_unmap_range(inode, start, end + 1 - start);

	error = filemap_write_and_wait_range(inode->i_mapping, start, end);
	if (error)
		return error;
	truncate_pagecache_range(inode, start, end);
	return 0;
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

	ASSERT(fuse_inode_has_iomap(inode));

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
	 * new EOF and bounce the new size out to userspace.
	 */
	if (new_size) {
		error = fuse_iomap_setsize_start(inode, new_size);
		if (error)
			return error;

		fuse_write_update_attr(inode, new_size, length);
	}

	file_update_time(file);
	return 0;
}

int fuse_dev_ioctl_iomap_support(struct file *file,
				 struct fuse_iomap_support __user *argp)
{
	struct fuse_iomap_support ios = { };

	if (fuse_iomap_enabled())
		ios.flags = FUSE_IOMAP_SUPPORT_FILEIO;

	if (copy_to_user(argp, &ios, sizeof(ios)))
		return -EFAULT;
	return 0;
}
