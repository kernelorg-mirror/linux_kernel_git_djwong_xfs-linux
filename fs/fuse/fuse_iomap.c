// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <linux/iomap.h>
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

	/* XXX: we don't support devices yet */
	if (BAD_DATA(map->dev != FUSE_IOMAP_DEV_NULL))
		return false;

	/* No overflows in the device range, if supplied */
	if (map->addr != FUSE_IOMAP_NULL_ADDR &&
	    BAD_DATA(check_add_overflow(map->addr, map->length, &end)))
		return false;

	return true;
}

/* Convert a mapping from the server into something the kernel can use */
static inline void fuse_iomap_from_server(struct iomap *iomap,
					  const struct fuse_iomap_io *fmap)
{
	iomap->addr = fmap->addr;
	iomap->offset = fmap->offset;
	iomap->length = fmap->length;
	iomap->type = fuse_iomap_type_from_server(fmap->type);
	iomap->flags = fuse_iomap_flags_from_server(fmap->flags);
	iomap->bdev = NULL; /* XXX */
}

/* Convert a mapping from the kernel into something the server can use */
static inline void fuse_iomap_to_server(struct fuse_iomap_io *fmap,
					const struct iomap *iomap)
{
	fmap->addr = iomap->addr;
	fmap->offset = iomap->offset;
	fmap->length = iomap->length;
	fmap->type = fuse_iomap_type_to_server(iomap->type);
	fmap->flags = fuse_iomap_flags_to_server(iomap->flags);
	fmap->dev = FUSE_IOMAP_DEV_NULL; /* XXX */
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
	FUSE_ARGS(args);
	int err;

	args.opcode = FUSE_IOMAP_BEGIN;
	args.nodeid = get_node_id(inode);
	args.in_numargs = 1;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	args.out_numargs = 1;
	args.out_args[0].size = sizeof(outarg);
	args.out_args[0].value = &outarg;
	err = fuse_simple_request(fm, &args);
	if (err)
		return err;

	err = fuse_iomap_begin_validate(inode, opflags, pos, &outarg);
	if (err)
		return err;

	if (fuse_is_iomap_file_write(opflags) &&
	    outarg.write.type != FUSE_IOMAP_TYPE_PURE_OVERWRITE) {
		/*
		 * For an out of place write, we must supply the write mapping
		 * via @iomap, and the read mapping via @srcmap.
		 */
		fuse_iomap_from_server(iomap, &outarg.write);
		fuse_iomap_from_server(srcmap, &outarg.read);
	} else {
		/*
		 * For everything else (reads, reporting, and pure overwrites),
		 * we can return the sole mapping through @iomap and leave
		 * @srcmap unchanged from its default (HOLE).
		 */
		fuse_iomap_from_server(iomap, &outarg.read);
	}

	return 0;
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

		fuse_iomap_to_server(&inarg.map, iomap);

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
		if (err)
			return err;
	}

	return 0;
}

const struct iomap_ops fuse_iomap_ops = {
	.iomap_begin		= fuse_iomap_begin,
	.iomap_end		= fuse_iomap_end,
};
