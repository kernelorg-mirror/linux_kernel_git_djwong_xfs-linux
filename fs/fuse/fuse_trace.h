/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM fuse

#if !defined(_TRACE_FUSE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_FUSE_H

#include <linux/tracepoint.h>

#define OPCODES							\
	EM( FUSE_LOOKUP,		"FUSE_LOOKUP")		\
	EM( FUSE_FORGET,		"FUSE_FORGET")		\
	EM( FUSE_GETATTR,		"FUSE_GETATTR")		\
	EM( FUSE_SETATTR,		"FUSE_SETATTR")		\
	EM( FUSE_READLINK,		"FUSE_READLINK")	\
	EM( FUSE_SYMLINK,		"FUSE_SYMLINK")		\
	EM( FUSE_MKNOD,			"FUSE_MKNOD")		\
	EM( FUSE_MKDIR,			"FUSE_MKDIR")		\
	EM( FUSE_UNLINK,		"FUSE_UNLINK")		\
	EM( FUSE_RMDIR,			"FUSE_RMDIR")		\
	EM( FUSE_RENAME,		"FUSE_RENAME")		\
	EM( FUSE_LINK,			"FUSE_LINK")		\
	EM( FUSE_OPEN,			"FUSE_OPEN")		\
	EM( FUSE_READ,			"FUSE_READ")		\
	EM( FUSE_WRITE,			"FUSE_WRITE")		\
	EM( FUSE_STATFS,		"FUSE_STATFS")		\
	EM( FUSE_RELEASE,		"FUSE_RELEASE")		\
	EM( FUSE_FSYNC,			"FUSE_FSYNC")		\
	EM( FUSE_SETXATTR,		"FUSE_SETXATTR")	\
	EM( FUSE_GETXATTR,		"FUSE_GETXATTR")	\
	EM( FUSE_LISTXATTR,		"FUSE_LISTXATTR")	\
	EM( FUSE_REMOVEXATTR,		"FUSE_REMOVEXATTR")	\
	EM( FUSE_FLUSH,			"FUSE_FLUSH")		\
	EM( FUSE_INIT,			"FUSE_INIT")		\
	EM( FUSE_OPENDIR,		"FUSE_OPENDIR")		\
	EM( FUSE_READDIR,		"FUSE_READDIR")		\
	EM( FUSE_RELEASEDIR,		"FUSE_RELEASEDIR")	\
	EM( FUSE_FSYNCDIR,		"FUSE_FSYNCDIR")	\
	EM( FUSE_GETLK,			"FUSE_GETLK")		\
	EM( FUSE_SETLK,			"FUSE_SETLK")		\
	EM( FUSE_SETLKW,		"FUSE_SETLKW")		\
	EM( FUSE_ACCESS,		"FUSE_ACCESS")		\
	EM( FUSE_CREATE,		"FUSE_CREATE")		\
	EM( FUSE_INTERRUPT,		"FUSE_INTERRUPT")	\
	EM( FUSE_BMAP,			"FUSE_BMAP")		\
	EM( FUSE_DESTROY,		"FUSE_DESTROY")		\
	EM( FUSE_IOCTL,			"FUSE_IOCTL")		\
	EM( FUSE_POLL,			"FUSE_POLL")		\
	EM( FUSE_NOTIFY_REPLY,		"FUSE_NOTIFY_REPLY")	\
	EM( FUSE_BATCH_FORGET,		"FUSE_BATCH_FORGET")	\
	EM( FUSE_FALLOCATE,		"FUSE_FALLOCATE")	\
	EM( FUSE_READDIRPLUS,		"FUSE_READDIRPLUS")	\
	EM( FUSE_RENAME2,		"FUSE_RENAME2")		\
	EM( FUSE_LSEEK,			"FUSE_LSEEK")		\
	EM( FUSE_COPY_FILE_RANGE,	"FUSE_COPY_FILE_RANGE")	\
	EM( FUSE_SETUPMAPPING,		"FUSE_SETUPMAPPING")	\
	EM( FUSE_REMOVEMAPPING,		"FUSE_REMOVEMAPPING")	\
	EM( FUSE_SYNCFS,		"FUSE_SYNCFS")		\
	EM( FUSE_TMPFILE,		"FUSE_TMPFILE")		\
	EM( FUSE_STATX,			"FUSE_STATX")		\
	EM( FUSE_IOMAP_CONFIG,		"FUSE_IOMAP_CONFIG")	\
	EM( FUSE_IOMAP_BEGIN,		"FUSE_IOMAP_BEGIN")	\
	EM( FUSE_IOMAP_END,		"FUSE_IOMAP_END")	\
	EM( FUSE_IOMAP_IOEND,		"FUSE_IOMAP_IOEND")	\
	EMe(CUSE_INIT,			"CUSE_INIT")

/*
 * This will turn the above table into TRACE_DEFINE_ENUM() for each of the
 * entries.
 */
#undef EM
#undef EMe
#define EM(a, b)	TRACE_DEFINE_ENUM(a);
#define EMe(a, b)	TRACE_DEFINE_ENUM(a);

OPCODES

/* Now we redfine it with the table that __print_symbolic needs. */
#undef EM
#undef EMe
#define EM(a, b)	{a, b},
#define EMe(a, b)	{a, b}

/* tracepoint boilerplate so we don't have to keep doing this */
#define FUSE_INODE_FIELDS \
		__field(dev_t,			connection) \
		__field(uint64_t,		ino) \
		__field(uint64_t,		nodeid) \
		__field(loff_t,			isize)

#define FUSE_INODE_ASSIGN(inode, fi, fm) \
		const struct fuse_inode *fi = get_fuse_inode_c(inode); \
		const struct fuse_mount *fm = get_fuse_mount_c(inode); \
\
		__entry->connection	=	(fm)->fc->dev; \
		__entry->ino		=	(fi)->orig_ino; \
		__entry->nodeid		=	(fi)->nodeid; \
		__entry->isize		=	i_size_read(inode)

#define FUSE_INODE_FMT \
		"connection %u ino %llu nodeid %llu isize 0x%llx"

#define FUSE_INODE_PRINTK_ARGS \
		__entry->connection, \
		__entry->ino, \
		__entry->nodeid, \
		__entry->isize

#define FUSE_FILE_RANGE_FIELDS(prefix) \
		__field(loff_t,			prefix##offset) \
		__field(loff_t,			prefix##length)

#define FUSE_FILE_RANGE_FMT(prefix) \
		" " prefix "pos 0x%llx length 0x%llx"

#define FUSE_FILE_RANGE_PRINTK_ARGS(prefix) \
		__entry->prefix##offset, \
		__entry->prefix##length

/* combinations of boilerplate to reduce typing further */
#define FUSE_IO_RANGE_FIELDS(prefix) \
		FUSE_INODE_FIELDS \
		FUSE_FILE_RANGE_FIELDS(prefix)

#define FUSE_IO_RANGE_FMT(prefix) \
		FUSE_INODE_FMT FUSE_FILE_RANGE_FMT(prefix)

#define FUSE_IO_RANGE_PRINTK_ARGS(prefix) \
		FUSE_INODE_PRINTK_ARGS, \
		FUSE_FILE_RANGE_PRINTK_ARGS(prefix)

TRACE_EVENT(fuse_request_send,
	TP_PROTO(const struct fuse_req *req),

	TP_ARGS(req),

	TP_STRUCT__entry(
		__field(dev_t,			connection)
		__field(uint64_t,		unique)
		__field(enum fuse_opcode,	opcode)
		__field(uint32_t,		len)
	),

	TP_fast_assign(
		__entry->connection	=	req->fm->fc->dev;
		__entry->unique		=	req->in.h.unique;
		__entry->opcode		=	req->in.h.opcode;
		__entry->len		=	req->in.h.len;
	),

	TP_printk("connection %u req %llu opcode %u (%s) len %u ",
		  __entry->connection, __entry->unique, __entry->opcode,
		  __print_symbolic(__entry->opcode, OPCODES), __entry->len)
);

TRACE_EVENT(fuse_request_end,
	TP_PROTO(const struct fuse_req *req),

	TP_ARGS(req),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	unique)
		__field(uint32_t,	len)
		__field(int32_t,	error)
	),

	TP_fast_assign(
		__entry->connection	=	req->fm->fc->dev;
		__entry->unique		=	req->in.h.unique;
		__entry->len		=	req->out.h.len;
		__entry->error		=	req->out.h.error;
	),

	TP_printk("connection %u req %llu len %u error %d", __entry->connection,
		  __entry->unique, __entry->len, __entry->error)
);

DECLARE_EVENT_CLASS(fuse_fileattr_class,
	TP_PROTO(const struct inode *inode, unsigned int old_iflags),

	TP_ARGS(inode, old_iflags),

	TP_STRUCT__entry(
		FUSE_INODE_FIELDS
		__field(unsigned int,		old_iflags)
		__field(unsigned int,		new_iflags)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->old_iflags	=	old_iflags;
		__entry->new_iflags	=	inode->i_flags;
	),

	TP_printk(FUSE_INODE_FMT " old_iflags 0x%x iflags 0x%x",
		  FUSE_INODE_PRINTK_ARGS,
		  __entry->old_iflags,
		  __entry->new_iflags)
);
#define DEFINE_FUSE_FILEATTR_EVENT(name)	\
DEFINE_EVENT(fuse_fileattr_class, name,		\
	TP_PROTO(const struct inode *inode, unsigned int old_iflags), \
	TP_ARGS(inode, old_iflags))
DEFINE_FUSE_FILEATTR_EVENT(fuse_fileattr_update_inode);
DEFINE_FUSE_FILEATTR_EVENT(fuse_fileattr_init);

TRACE_EVENT(fuse_setattr_fill,
	TP_PROTO(const struct inode *inode,
		 const struct fuse_setattr_in *inarg),
	TP_ARGS(inode, inarg),

	TP_STRUCT__entry(
		FUSE_INODE_FIELDS
		__field(umode_t,		mode)
		__field(uint32_t,		valid)
		__field(umode_t,		new_mode)
		__field(uint64_t,		new_size)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->mode		=	inode->i_mode;
		__entry->valid		=	inarg->valid;
		__entry->new_mode	=	inarg->mode;
		__entry->new_size	=	inarg->size;
	),

	TP_printk(FUSE_INODE_FMT " mode 0%o valid 0x%x new_mode 0%o new_size 0x%llx",
		  FUSE_INODE_PRINTK_ARGS,
		  __entry->mode,
		  __entry->valid,
		  __entry->new_mode,
		  __entry->new_size)
);

TRACE_EVENT(fuse_setattr,
	TP_PROTO(const struct inode *inode,
		 const struct iattr *inarg),
	TP_ARGS(inode, inarg),

	TP_STRUCT__entry(
		FUSE_INODE_FIELDS
		__field(umode_t,		mode)
		__field(uint32_t,		valid)
		__field(umode_t,		new_mode)
		__field(uint64_t,		new_size)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->mode		=	inode->i_mode;
		__entry->valid		=	inarg->ia_valid;
		__entry->new_mode	=	inarg->ia_mode;
		__entry->new_size	=	inarg->ia_size;
	),

	TP_printk(FUSE_INODE_FMT " mode 0%o valid 0x%x new_mode 0%o new_size 0x%llx",
		  FUSE_INODE_PRINTK_ARGS,
		  __entry->mode,
		  __entry->valid,
		  __entry->new_mode,
		  __entry->new_size)
);

#ifdef CONFIG_FUSE_BACKING
#define FUSE_BACKING_FLAG_STRINGS \
	{ FUSE_BACKING_TYPE_PASSTHROUGH,	"pass" }, \
	{ FUSE_BACKING_TYPE_IOMAP,		"iomap" }

TRACE_EVENT(fuse_backing_class,
	TP_PROTO(const struct fuse_conn *fc, unsigned int idx,
		 const struct fuse_backing *fb),

	TP_ARGS(fc, idx, fb),

	TP_STRUCT__entry(
		__field(dev_t,			connection)
		__field(unsigned int,		idx)
		__field(unsigned int,		type)
		__field(unsigned long,		ino)
		__field(dev_t,			rdev)
	),

	TP_fast_assign(
		struct inode *inode = file_inode(fb->file);

		__entry->connection	=	fc->dev;
		__entry->idx		=	idx;
		__entry->ino		=	inode->i_ino;
		__entry->type		=	fb->ops->type;
		if (fb->ops->type == FUSE_BACKING_TYPE_IOMAP)
			__entry->rdev	=	inode->i_rdev;
		else
			__entry->rdev	=	0;
	),

	TP_printk("connection %u idx %u type %s ino 0x%lx rdev %u:%u",
		  __entry->connection,
		  __entry->idx,
		  __print_symbolic(__entry->type, FUSE_BACKING_FLAG_STRINGS),
		  __entry->ino,
		  MAJOR(__entry->rdev), MINOR(__entry->rdev))
);
#define DEFINE_FUSE_BACKING_EVENT(name)		\
DEFINE_EVENT(fuse_backing_class, name,		\
	TP_PROTO(const struct fuse_conn *fc, unsigned int idx, \
		 const struct fuse_backing *fb), \
	TP_ARGS(fc, idx, fb))
DEFINE_FUSE_BACKING_EVENT(fuse_backing_open);
DEFINE_FUSE_BACKING_EVENT(fuse_backing_close);
#endif /* CONFIG_FUSE_BACKING */

#if IS_ENABLED(CONFIG_FUSE_IOMAP)
struct iomap_writepage_ctx;
struct iomap_ioend;
struct iomap;
struct fuse_iext_cursor;
struct fuse_iomap_lookup;

/* tracepoint boilerplate so we don't have to keep doing this */
#define FUSE_IOMAP_OPFLAGS_FIELD \
		__field(unsigned,		opflags)

#define FUSE_IOMAP_OPFLAGS_FMT \
		" opflags (%s)"

#define FUSE_IOMAP_OPFLAGS_PRINTK_ARG \
		__print_flags(__entry->opflags, "|", FUSE_IOMAP_OP_STRINGS)

#define FUSE_IOMAP_MAP_FIELDS(prefix) \
		__field(uint64_t,		prefix##offset) \
		__field(uint64_t,		prefix##length) \
		__field(uint64_t,		prefix##addr) \
		__field(uint32_t,		prefix##dev) \
		__field(uint16_t,		prefix##type) \
		__field(uint16_t,		prefix##flags)

#define FUSE_IOMAP_MAP_FMT(prefix) \
		" " prefix "offset 0x%llx length 0x%llx type %s dev %u addr 0x%llx mapflags (%s)"

#define FUSE_IOMAP_MAP_PRINTK_ARGS(prefix) \
		__entry->prefix##offset, \
		__entry->prefix##length, \
		__print_symbolic(__entry->prefix##type, FUSE_IOMAP_TYPE_STRINGS), \
		__entry->prefix##dev, \
		__entry->prefix##addr, \
		__print_flags(__entry->prefix##flags, "|", FUSE_IOMAP_F_STRINGS)

#define FUSE_IOMAP_IODIR_FIELD \
		__field(enum fuse_iomap_iodir,	iodir)

#define FUSE_IOMAP_IODIR_FMT \
		 " iodir %s"

#define FUSE_IOMAP_IODIR_PRINTK_ARGS \
		  __print_symbolic(__entry->iodir, FUSE_IOMAP_FORK_STRINGS)


/* combinations of boilerplate to reduce typing further */
#define FUSE_IOMAP_OP_FIELDS(prefix) \
		FUSE_INODE_FIELDS \
		FUSE_IOMAP_OPFLAGS_FIELD \
		FUSE_FILE_RANGE_FIELDS(prefix)

#define FUSE_IOMAP_OP_FMT(prefix) \
		FUSE_INODE_FMT FUSE_IOMAP_OPFLAGS_FMT FUSE_FILE_RANGE_FMT(prefix)

#define FUSE_IOMAP_OP_PRINTK_ARGS(prefix) \
		FUSE_INODE_PRINTK_ARGS, \
		FUSE_IOMAP_OPFLAGS_PRINTK_ARG, \
		FUSE_FILE_RANGE_PRINTK_ARGS(prefix)

/* string decoding */
#define FUSE_IOMAP_F_STRINGS \
	{ FUSE_IOMAP_F_NEW,			"new" }, \
	{ FUSE_IOMAP_F_DIRTY,			"dirty" }, \
	{ FUSE_IOMAP_F_SHARED,			"shared" }, \
	{ FUSE_IOMAP_F_MERGED,			"merged" }, \
	{ FUSE_IOMAP_F_BOUNDARY,		"boundary" }, \
	{ FUSE_IOMAP_F_ANON_WRITE,		"anon_write" }, \
	{ FUSE_IOMAP_F_ATOMIC_BIO,		"atomic" }, \
	{ FUSE_IOMAP_F_WANT_IOMAP_END,		"iomap_end" }, \
	{ FUSE_IOMAP_F_SIZE_CHANGED,		"append" }, \
	{ FUSE_IOMAP_F_STALE,			"stale" }

#define FUSE_IOMAP_OP_STRINGS \
	{ FUSE_IOMAP_OP_WRITE,			"write" }, \
	{ FUSE_IOMAP_OP_ZERO,			"zero" }, \
	{ FUSE_IOMAP_OP_REPORT,			"report" }, \
	{ FUSE_IOMAP_OP_FAULT,			"fault" }, \
	{ FUSE_IOMAP_OP_DIRECT,			"direct" }, \
	{ FUSE_IOMAP_OP_NOWAIT,			"nowait" }, \
	{ FUSE_IOMAP_OP_OVERWRITE_ONLY,		"overwrite" }, \
	{ FUSE_IOMAP_OP_UNSHARE,		"unshare" }, \
	{ FUSE_IOMAP_OP_DAX,			"fsdax" }, \
	{ FUSE_IOMAP_OP_ATOMIC,			"atomic" }, \
	{ FUSE_IOMAP_OP_DONTCACHE,		"dontcache" }, \
	{ FUSE_IOMAP_OP_WRITEBACK,		"writeback" }

#define FUSE_IOMAP_TYPE_STRINGS \
	{ FUSE_IOMAP_TYPE_PURE_OVERWRITE,	"overwrite" }, \
	{ FUSE_IOMAP_TYPE_RETRY_CACHE,		"retry" }, \
	{ FUSE_IOMAP_TYPE_HOLE,			"hole" }, \
	{ FUSE_IOMAP_TYPE_DELALLOC,		"delalloc" }, \
	{ FUSE_IOMAP_TYPE_MAPPED,		"mapped" }, \
	{ FUSE_IOMAP_TYPE_UNWRITTEN,		"unwritten" }, \
	{ FUSE_IOMAP_TYPE_INLINE,		"inline" }

#define FUSE_IOMAP_IOEND_STRINGS \
	{ FUSE_IOMAP_IOEND_SHARED,		"shared" }, \
	{ FUSE_IOMAP_IOEND_UNWRITTEN,		"unwritten" }, \
	{ FUSE_IOMAP_IOEND_BOUNDARY,		"boundary" }, \
	{ FUSE_IOMAP_IOEND_DIRECT,		"direct" }, \
	{ FUSE_IOMAP_IOEND_APPEND,		"append" }, \
	{ FUSE_IOMAP_IOEND_WRITEBACK,		"writeback" }

#define IOMAP_DIOEND_STRINGS \
	{ IOMAP_DIO_UNWRITTEN,			"unwritten" }, \
	{ IOMAP_DIO_COW,			"cow" }

TRACE_DEFINE_ENUM(FUSE_I_ADVISE_RDPLUS);
TRACE_DEFINE_ENUM(FUSE_I_INIT_RDPLUS);
TRACE_DEFINE_ENUM(FUSE_I_SIZE_UNSTABLE);
TRACE_DEFINE_ENUM(FUSE_I_BAD);
TRACE_DEFINE_ENUM(FUSE_I_BTIME);
TRACE_DEFINE_ENUM(FUSE_I_CACHE_IO_MODE);
TRACE_DEFINE_ENUM(FUSE_I_IOMAP);
TRACE_DEFINE_ENUM(FUSE_I_ATOMIC);
TRACE_DEFINE_ENUM(FUSE_I_IOMAP_CACHE);

#define FUSE_IFLAG_STRINGS \
	{ 1 << FUSE_I_ADVISE_RDPLUS,		"advise_rdplus" }, \
	{ 1 << FUSE_I_INIT_RDPLUS,		"init_rdplus" }, \
	{ 1 << FUSE_I_SIZE_UNSTABLE,		"size_unstable" }, \
	{ 1 << FUSE_I_BAD,			"bad" }, \
	{ 1 << FUSE_I_BTIME,			"btime" }, \
	{ 1 << FUSE_I_CACHE_IO_MODE,		"cacheio" }, \
	{ 1 << FUSE_I_IOMAP,			"iomap" }, \
	{ 1 << FUSE_I_ATOMIC,			"atomic" }, \
	{ 1 << FUSE_I_IOMAP_CACHE,		"iomap_cache" }

#define IOMAP_IOEND_STRINGS \
	{ IOMAP_IOEND_SHARED,			"shared" }, \
	{ IOMAP_IOEND_UNWRITTEN,		"unwritten" }, \
	{ IOMAP_IOEND_BOUNDARY,			"boundary" }, \
	{ IOMAP_IOEND_DIRECT,			"direct" }

#define FUSE_IOMAP_CONFIG_STRINGS \
	{ FUSE_IOMAP_CONFIG_SID,		"sid" }, \
	{ FUSE_IOMAP_CONFIG_UUID,		"uuid" }, \
	{ FUSE_IOMAP_CONFIG_BLOCKSIZE,		"blocksize" }, \
	{ FUSE_IOMAP_CONFIG_MAX_LINKS,		"max_links" }, \
	{ FUSE_IOMAP_CONFIG_TIME,		"time" }, \
	{ FUSE_IOMAP_CONFIG_MAXBYTES,		"maxbytes" }

TRACE_DEFINE_ENUM(READ_MAPPING);
TRACE_DEFINE_ENUM(WRITE_MAPPING);

#define FUSE_IOMAP_FORK_STRINGS \
	{ READ_MAPPING,				"read" }, \
	{ WRITE_MAPPING,			"write" }

#define FUSE_IEXT_STATE_STRINGS \
	{ FUSE_IEXT_LEFT_CONTIG,		"l_cont" }, \
	{ FUSE_IEXT_RIGHT_CONTIG,		"r_cont" }, \
	{ FUSE_IEXT_LEFT_FILLING,		"l_fill" }, \
	{ FUSE_IEXT_RIGHT_FILLING,		"r_fill" }, \
	{ FUSE_IEXT_LEFT_VALID,			"l_valid" }, \
	{ FUSE_IEXT_RIGHT_VALID,		"r_valid" }, \
	{ FUSE_IEXT_WRITE_MAPPING,		"write" }

DECLARE_EVENT_CLASS(fuse_iomap_check_class,
	TP_PROTO(const char *func, int line, const char *condition),

	TP_ARGS(func, line, condition),

	TP_STRUCT__entry(
		__string(func,			func)
		__field(int,			line)
		__string(condition,		condition)
	),

	TP_fast_assign(
		__assign_str(func);
		__assign_str(condition);
		__entry->line		=	line;
	),

	TP_printk("func %s line %d condition %s", __get_str(func),
		  __entry->line, __get_str(condition))
);
#define DEFINE_FUSE_IOMAP_CHECK_EVENT(name)	\
DEFINE_EVENT(fuse_iomap_check_class, name,	\
	TP_PROTO(const char *func, int line, const char *condition), \
	TP_ARGS(func, line, condition))
#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG)
DEFINE_FUSE_IOMAP_CHECK_EVENT(fuse_iomap_assert);
#endif
DEFINE_FUSE_IOMAP_CHECK_EVENT(fuse_iomap_bad_data);

TRACE_EVENT(fuse_iomap_begin,
	TP_PROTO(const struct inode *inode, loff_t pos, loff_t count,
		 unsigned opflags),

	TP_ARGS(inode, pos, count, opflags),

	TP_STRUCT__entry(
		FUSE_IOMAP_OP_FIELDS()
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	pos;
		__entry->length		=	count;
		__entry->opflags	=	opflags;
	),

	TP_printk(FUSE_IOMAP_OP_FMT(),
		  FUSE_IOMAP_OP_PRINTK_ARGS())
);

TRACE_EVENT(fuse_iomap_begin_error,
	TP_PROTO(const struct inode *inode, loff_t pos, loff_t count,
		 unsigned opflags, int error),

	TP_ARGS(inode, pos, count, opflags, error),

	TP_STRUCT__entry(
		FUSE_IOMAP_OP_FIELDS()
		__field(int,			error)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	pos;
		__entry->length		=	count;
		__entry->opflags	=	opflags;
		__entry->error		=	error;
	),

	TP_printk(FUSE_IOMAP_OP_FMT() " err %d",
		  FUSE_IOMAP_OP_PRINTK_ARGS(),
		  __entry->error)
);

DECLARE_EVENT_CLASS(fuse_iomap_mapping_class,
	TP_PROTO(const struct inode *inode, const struct fuse_iomap_io *map),

	TP_ARGS(inode, map),

	TP_STRUCT__entry(
		FUSE_INODE_FIELDS
		FUSE_IOMAP_MAP_FIELDS(map)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->mapoffset	=	map->offset;
		__entry->maplength	=	map->length;
		__entry->mapdev		=	map->dev;
		__entry->mapaddr	=	map->addr;
		__entry->maptype	=	map->type;
		__entry->mapflags	=	map->flags;
	),

	TP_printk(FUSE_INODE_FMT FUSE_IOMAP_MAP_FMT(),
		  FUSE_INODE_PRINTK_ARGS,
		  FUSE_IOMAP_MAP_PRINTK_ARGS(map))
);
#define DEFINE_FUSE_IOMAP_MAPPING_EVENT(name)	\
DEFINE_EVENT(fuse_iomap_mapping_class, name,	\
	TP_PROTO(const struct inode *inode, const struct fuse_iomap_io *map), \
	TP_ARGS(inode, map))
DEFINE_FUSE_IOMAP_MAPPING_EVENT(fuse_iomap_read_map);
DEFINE_FUSE_IOMAP_MAPPING_EVENT(fuse_iomap_write_map);

TRACE_EVENT(fuse_iomap_end,
	TP_PROTO(const struct inode *inode,
		 const struct fuse_iomap_end_in *inarg),

	TP_ARGS(inode, inarg),

	TP_STRUCT__entry(
		FUSE_IOMAP_OP_FIELDS()
		__field(size_t,			written)
		FUSE_IOMAP_MAP_FIELDS(map)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->opflags	=	inarg->opflags;
		__entry->written	=	inarg->written;
		__entry->offset		=	inarg->pos;
		__entry->length		=	inarg->count;

		__entry->mapoffset	=	inarg->map.offset;
		__entry->maplength	=	inarg->map.length;
		__entry->mapdev		=	inarg->map.dev;
		__entry->mapaddr	=	inarg->map.addr;
		__entry->maptype	=	inarg->map.type;
		__entry->mapflags	=	inarg->map.flags;
	),

	TP_printk(FUSE_IOMAP_OP_FMT() " written %zd" FUSE_IOMAP_MAP_FMT(),
		  FUSE_IOMAP_OP_PRINTK_ARGS(),
		  __entry->written,
		  FUSE_IOMAP_MAP_PRINTK_ARGS(map))
);

TRACE_EVENT(fuse_iomap_end_error,
	TP_PROTO(const struct inode *inode,
		 const struct fuse_iomap_end_in *inarg, int error),

	TP_ARGS(inode, inarg, error),

	TP_STRUCT__entry(
		FUSE_IOMAP_OP_FIELDS()
		__field(size_t,			written)
		__field(int,			error)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	inarg->pos;
		__entry->length		=	inarg->count;
		__entry->opflags	=	inarg->opflags;
		__entry->written	=	inarg->written;
		__entry->error		=	error;
	),

	TP_printk(FUSE_IOMAP_OP_FMT() " written %zd error %d",
		  FUSE_IOMAP_OP_PRINTK_ARGS(),
		  __entry->written,
		  __entry->error)
);

TRACE_EVENT(fuse_iomap_ioend,
	TP_PROTO(const struct inode *inode,
		 const struct fuse_iomap_ioend_in *inarg),

	TP_ARGS(inode, inarg),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		__field(unsigned,		ioendflags)
		__field(int,			error)
		__field(uint64_t,		new_addr)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	inarg->pos;
		__entry->length		=	inarg->written;
		__entry->ioendflags	=	inarg->ioendflags;
		__entry->error		=	inarg->error;
		__entry->new_addr	=	inarg->new_addr;
	),

	TP_printk(FUSE_IO_RANGE_FMT() " ioendflags (%s) error %d new_addr 0x%llx",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  __print_flags(__entry->ioendflags, "|", FUSE_IOMAP_IOEND_STRINGS),
		  __entry->error,
		  __entry->new_addr)
);

TRACE_EVENT(fuse_iomap_ioend_error,
	TP_PROTO(const struct inode *inode,
		 const struct fuse_iomap_ioend_in *inarg,
		 int error),

	TP_ARGS(inode, inarg, error),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		__field(unsigned,		ioendflags)
		__field(int,			error)
		__field(uint64_t,		new_addr)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	inarg->pos;
		__entry->length		=	inarg->written;
		__entry->ioendflags	=	inarg->ioendflags;
		__entry->error		=	error;
		__entry->new_addr	=	inarg->new_addr;
	),

	TP_printk(FUSE_IO_RANGE_FMT() " ioendflags (%s) error %d new_addr 0x%llx",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  __print_flags(__entry->ioendflags, "|", FUSE_IOMAP_IOEND_STRINGS),
		  __entry->error,
		  __entry->new_addr)
);

TRACE_EVENT(fuse_iomap_dev_add,
	TP_PROTO(const struct fuse_conn *fc,
		 const struct fuse_backing_map *map),

	TP_ARGS(fc, map),

	TP_STRUCT__entry(
		__field(dev_t,			connection)
		__field(int,			fd)
		__field(unsigned int,		flags)
	),

	TP_fast_assign(
		__entry->connection	=	fc->dev;
		__entry->fd		=	map->fd;
		__entry->flags		=	map->flags;
	),

	TP_printk("connection %u fd %d flags 0x%x",
		  __entry->connection,
		  __entry->fd,
		  __entry->flags)
);

TRACE_EVENT(fuse_iomap_fiemap,
	TP_PROTO(const struct inode *inode, u64 start, u64 count,
		unsigned int flags),

	TP_ARGS(inode, start, count, flags),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		__field(unsigned int,		flags)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	start;
		__entry->length		=	count;
		__entry->flags		=	flags;
	),

	TP_printk(FUSE_IO_RANGE_FMT("fiemap") " flags 0x%x",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  __entry->flags)
);

TRACE_EVENT(fuse_iomap_lseek,
	TP_PROTO(const struct inode *inode, loff_t offset, int whence),

	TP_ARGS(inode, offset, whence),

	TP_STRUCT__entry(
		FUSE_INODE_FIELDS
		__field(loff_t,			offset)
		__field(int,			whence)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	offset;
		__entry->whence		=	whence;
	),

	TP_printk(FUSE_INODE_FMT " offset 0x%llx whence %d",
		  FUSE_INODE_PRINTK_ARGS,
		  __entry->offset,
		  __entry->whence)
);

DECLARE_EVENT_CLASS(fuse_iomap_file_io_class,
	TP_PROTO(const struct kiocb *iocb, const struct iov_iter *iter),
	TP_ARGS(iocb, iter),
	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
	),
	TP_fast_assign(
		FUSE_INODE_ASSIGN(file_inode(iocb->ki_filp), fi, fm);
		__entry->offset		=	iocb->ki_pos;
		__entry->length		=	iov_iter_count(iter);
	),
	TP_printk(FUSE_IO_RANGE_FMT(),
		  FUSE_IO_RANGE_PRINTK_ARGS())
)
#define DEFINE_FUSE_IOMAP_FILE_IO_EVENT(name)		\
DEFINE_EVENT(fuse_iomap_file_io_class, name,		\
	TP_PROTO(const struct kiocb *iocb, const struct iov_iter *iter), \
	TP_ARGS(iocb, iter))
DEFINE_FUSE_IOMAP_FILE_IO_EVENT(fuse_iomap_direct_read);
DEFINE_FUSE_IOMAP_FILE_IO_EVENT(fuse_iomap_direct_write);
DEFINE_FUSE_IOMAP_FILE_IO_EVENT(fuse_iomap_buffered_read);
DEFINE_FUSE_IOMAP_FILE_IO_EVENT(fuse_iomap_buffered_write);
DEFINE_FUSE_IOMAP_FILE_IO_EVENT(fuse_iomap_write_zero_eof);

DECLARE_EVENT_CLASS(fuse_iomap_file_ioend_class,
	TP_PROTO(const struct kiocb *iocb, const struct iov_iter *iter,
		 ssize_t ret),
	TP_ARGS(iocb, iter, ret),
	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		__field(ssize_t,		ret)
	),
	TP_fast_assign(
		FUSE_INODE_ASSIGN(file_inode(iocb->ki_filp), fi, fm);
		__entry->offset		=	iocb->ki_pos;
		__entry->length		=	iov_iter_count(iter);
		__entry->ret		=	ret;
	),
	TP_printk(FUSE_IO_RANGE_FMT() " ret 0x%zx",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  __entry->ret)
)
#define DEFINE_FUSE_IOMAP_FILE_IOEND_EVENT(name)	\
DEFINE_EVENT(fuse_iomap_file_ioend_class, name,		\
	TP_PROTO(const struct kiocb *iocb, const struct iov_iter *iter, \
		 ssize_t ret), \
	TP_ARGS(iocb, iter, ret))
DEFINE_FUSE_IOMAP_FILE_IOEND_EVENT(fuse_iomap_direct_read_end);
DEFINE_FUSE_IOMAP_FILE_IOEND_EVENT(fuse_iomap_direct_write_end);
DEFINE_FUSE_IOMAP_FILE_IOEND_EVENT(fuse_iomap_buffered_read_end);
DEFINE_FUSE_IOMAP_FILE_IOEND_EVENT(fuse_iomap_buffered_write_end);

TRACE_EVENT(fuse_iomap_dio_write_end_io,
	TP_PROTO(const struct inode *inode, loff_t pos, ssize_t written,
		 int error, unsigned flags),

	TP_ARGS(inode, pos, written, error, flags),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		__field(unsigned,		dioendflags)
		__field(int,			error)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	pos;
		__entry->length		=	written;
		__entry->dioendflags	=	flags;
		__entry->error		=	error;
	),

	TP_printk(FUSE_IO_RANGE_FMT() " dioendflags (%s) error %d",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  __print_flags(__entry->dioendflags, "|", IOMAP_DIOEND_STRINGS),
		  __entry->error)
);

DECLARE_EVENT_CLASS(fuse_inode_state_class,
	TP_PROTO(const struct inode *inode),
	TP_ARGS(inode),

	TP_STRUCT__entry(
		FUSE_INODE_FIELDS
		__field(unsigned long,		state)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->state		=	fi->state;
	),

	TP_printk(FUSE_INODE_FMT " state (%s)",
		  FUSE_INODE_PRINTK_ARGS,
		  __print_flags(__entry->state, "|", FUSE_IFLAG_STRINGS))
);
#define DEFINE_FUSE_INODE_STATE_EVENT(name)	\
DEFINE_EVENT(fuse_inode_state_class, name,	\
	TP_PROTO(const struct inode *inode),	\
	TP_ARGS(inode))
DEFINE_FUSE_INODE_STATE_EVENT(fuse_iomap_init_inode);
DEFINE_FUSE_INODE_STATE_EVENT(fuse_iomap_evict_inode);

TRACE_EVENT(fuse_iomap_end_ioend,
	TP_PROTO(const struct iomap_ioend *ioend),

	TP_ARGS(ioend),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		__field(unsigned int,		ioendflags)
		__field(int,			error)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(ioend->io_inode, fi, fm);
		__entry->offset		=	ioend->io_offset;
		__entry->length		=	ioend->io_size;
		__entry->ioendflags	=	ioend->io_flags;
		__entry->error		=	blk_status_to_errno(ioend->io_bio.bi_status);
	),

	TP_printk(FUSE_IO_RANGE_FMT() " ioendflags (%s) error %d",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  __print_flags(__entry->ioendflags, "|", IOMAP_IOEND_STRINGS),
		  __entry->error)
);

TRACE_EVENT(fuse_iomap_writeback_range,
	TP_PROTO(const struct inode *inode, u64 offset, unsigned int count,
		 u64 end_pos),

	TP_ARGS(inode, offset, count, end_pos),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		__field(uint64_t,		end_pos)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	offset;
		__entry->length		=	count;
		__entry->end_pos	=	end_pos;
	),

	TP_printk(FUSE_IO_RANGE_FMT() " end_pos 0x%llx",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  __entry->end_pos)
);

TRACE_EVENT(fuse_iomap_writeback_submit,
	TP_PROTO(const struct iomap_writepage_ctx *wpc, int error),

	TP_ARGS(wpc, error),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		__field(unsigned int,		nr_folios)
		__field(uint64_t,		addr)
		__field(int,			error)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(wpc->inode, fi, fm);
		__entry->nr_folios	=	wpc->nr_folios;
		__entry->offset		=	wpc->iomap.offset;
		__entry->length		=	wpc->iomap.length;
		__entry->addr		=	wpc->iomap.addr << 9;
		__entry->error		=	error;
	),

	TP_printk(FUSE_IO_RANGE_FMT() " addr 0x%llx nr_folios %u error %d",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  __entry->addr,
		  __entry->nr_folios,
		  __entry->error)
);

TRACE_EVENT(fuse_iomap_discard_folio,
	TP_PROTO(const struct inode *inode, loff_t offset, size_t count),

	TP_ARGS(inode, offset, count),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	offset;
		__entry->length		=	count;
	),

	TP_printk(FUSE_IO_RANGE_FMT(),
		  FUSE_IO_RANGE_PRINTK_ARGS())
);

TRACE_EVENT(fuse_iomap_writepages,
	TP_PROTO(const struct inode *inode, const struct writeback_control *wbc),

	TP_ARGS(inode, wbc),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		__field(long,			nr_to_write)
		__field(bool,			sync_all)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	wbc->range_start;
		__entry->length		=	wbc->range_end - wbc->range_start + 1;
		__entry->nr_to_write	=	wbc->nr_to_write;
		__entry->sync_all	=	wbc->sync_mode == WB_SYNC_ALL;
	),

	TP_printk(FUSE_IO_RANGE_FMT() " nr_folios %ld sync_all? %d",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  __entry->nr_to_write,
		  __entry->sync_all)
);

TRACE_EVENT(fuse_iomap_read_folio,
	TP_PROTO(const struct folio *folio),

	TP_ARGS(folio),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(folio->mapping->host, fi, fm);
		__entry->offset		=	folio_pos(folio);
		__entry->length		=	folio_size(folio);
	),

	TP_printk(FUSE_IO_RANGE_FMT(),
		  FUSE_IO_RANGE_PRINTK_ARGS())
);

TRACE_EVENT(fuse_iomap_readahead,
	TP_PROTO(const struct readahead_control *rac),

	TP_ARGS(rac),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
	),

	TP_fast_assign(
		struct readahead_control *mutrac = (struct readahead_control *)rac;
		FUSE_INODE_ASSIGN(file_inode(rac->file), fi, fm);
		__entry->offset		=	readahead_pos(mutrac);
		__entry->length		=	readahead_length(mutrac);
	),

	TP_printk(FUSE_IO_RANGE_FMT(),
		  FUSE_IO_RANGE_PRINTK_ARGS())
);

TRACE_EVENT(fuse_iomap_page_mkwrite,
	TP_PROTO(const struct vm_fault *vmf),

	TP_ARGS(vmf),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
	),

	TP_fast_assign(
		struct folio *folio = page_folio(vmf->page);
		FUSE_INODE_ASSIGN(file_inode(vmf->vma->vm_file), fi, fm);
		__entry->offset		=	folio_pos(folio);
		__entry->length		=	folio_size(folio);
	),

	TP_printk(FUSE_IO_RANGE_FMT(),
		  FUSE_IO_RANGE_PRINTK_ARGS())
);

DECLARE_EVENT_CLASS(fuse_iomap_file_range_class,
	TP_PROTO(const struct inode *inode, loff_t offset, loff_t length),

	TP_ARGS(inode, offset, length),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	offset;
		__entry->length		=	length;
	),

	TP_printk(FUSE_IO_RANGE_FMT(),
		  FUSE_IO_RANGE_PRINTK_ARGS())
)
#define DEFINE_FUSE_IOMAP_FILE_RANGE_EVENT(name)		\
DEFINE_EVENT(fuse_iomap_file_range_class, name,		\
	TP_PROTO(const struct inode *inode, loff_t offset, loff_t length), \
	TP_ARGS(inode, offset, length))
DEFINE_FUSE_IOMAP_FILE_RANGE_EVENT(fuse_iomap_truncate_up);
DEFINE_FUSE_IOMAP_FILE_RANGE_EVENT(fuse_iomap_truncate_down);
DEFINE_FUSE_IOMAP_FILE_RANGE_EVENT(fuse_iomap_punch_range);
DEFINE_FUSE_IOMAP_FILE_RANGE_EVENT(fuse_iomap_setsize);
DEFINE_FUSE_IOMAP_FILE_RANGE_EVENT(fuse_iomap_flush_unmap_range);
DEFINE_FUSE_IOMAP_FILE_RANGE_EVENT(fuse_iomap_cache_invalidate_range);

TRACE_EVENT(fuse_iomap_fallocate,
	TP_PROTO(const struct inode *inode, int mode, loff_t offset,
		 loff_t length, loff_t newsize),
	TP_ARGS(inode, mode, offset, length, newsize),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		__field(loff_t,			newsize)
		__field(int,			mode)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	offset;
		__entry->length		=	length;
		__entry->mode		=	mode;
		__entry->newsize	=	newsize;
	),

	TP_printk(FUSE_IO_RANGE_FMT() " mode 0x%x newsize 0x%llx",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  __entry->mode,
		  __entry->newsize)
);

TRACE_EVENT(fuse_iomap_config,
	TP_PROTO(const struct fuse_mount *fm,
		 const struct fuse_iomap_config_out *outarg),
	TP_ARGS(fm, outarg),

	TP_STRUCT__entry(
		__field(dev_t,			connection)
		__field(uint64_t,		root_nodeid)

		__field(uint32_t,		flags)
		__field(uint32_t,		blocksize)
		__field(uint32_t,		max_links)
		__field(uint32_t,		time_gran)

		__field(int64_t,		time_min)
		__field(int64_t,		time_max)
		__field(int64_t,		maxbytes)
		__field(uint8_t,		uuid_len)
	),

	TP_fast_assign(
		__entry->connection	=	fm->fc->dev;
		__entry->root_nodeid	=	fm->fc->root_nodeid;
		__entry->flags		=	outarg->flags;
		__entry->blocksize	=	outarg->s_blocksize;
		__entry->max_links	=	outarg->s_max_links;
		__entry->time_gran	=	outarg->s_time_gran;
		__entry->time_min	=	outarg->s_time_min;
		__entry->time_max	=	outarg->s_time_max;
		__entry->maxbytes	=	outarg->s_maxbytes;
		__entry->uuid_len	=	outarg->s_uuid_len;
	),

	TP_printk("connection %u root_ino 0x%llx flags (%s) blocksize 0x%x max_links %u time_gran %u time_min %lld time_max %lld maxbytes 0x%llx uuid_len %u",
		  __entry->connection, __entry->root_nodeid,
		  __print_flags(__entry->flags, "|", FUSE_IOMAP_CONFIG_STRINGS),
		  __entry->blocksize, __entry->max_links, __entry->time_gran,
		  __entry->time_min, __entry->time_max, __entry->maxbytes,
		  __entry->uuid_len)
);

TRACE_EVENT(fuse_iomap_dev_inval,
	TP_PROTO(const struct fuse_conn *fc,
		 const struct fuse_iomap_dev_inval_out *arg),
	TP_ARGS(fc, arg),

	TP_STRUCT__entry(
		__field(dev_t,			connection)
		__field(int,			dev)
		__field(unsigned long long,	offset)
		__field(unsigned long long,	length)
	),

	TP_fast_assign(
		__entry->connection	=	fc->dev;
		__entry->dev		=	arg->dev;
		__entry->offset		=	arg->offset;
		__entry->length		=	arg->length;
	),

	TP_printk("connection %u dev %d offset 0x%llx length 0x%llx",
		  __entry->connection,
		  __entry->dev,
		  __entry->offset,
		  __entry->length)
);

DECLARE_EVENT_CLASS(fuse_iomap_inline_class,
	TP_PROTO(const struct inode *inode, loff_t pos, uint64_t count,
		 const struct iomap *map),
	TP_ARGS(inode, pos, count, map),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		FUSE_IOMAP_MAP_FIELDS(map)
		__field(bool,			has_buf)
		__field(uint64_t,		validity_cookie)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	pos;
		__entry->length		=	count;

		__entry->mapdev		=	FUSE_IOMAP_DEV_NULL;
		__entry->mapaddr	=	map->addr;
		__entry->mapoffset	=	map->offset;
		__entry->maplength	=	map->length;
		__entry->maptype	=	map->type;
		__entry->mapflags	=	map->flags;

		__entry->has_buf	=	map->inline_data != NULL;
		__entry->validity_cookie=	map->validity_cookie;
	),

	TP_printk(FUSE_IO_RANGE_FMT() FUSE_IOMAP_MAP_FMT() " has_buf? %d cookie 0x%llx",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  FUSE_IOMAP_MAP_PRINTK_ARGS(map),
		  __entry->has_buf,
		  __entry->validity_cookie)
);
#define DEFINE_FUSE_IOMAP_INLINE_EVENT(name)	\
DEFINE_EVENT(fuse_iomap_inline_class, name,	\
	TP_PROTO(const struct inode *inode, loff_t pos, uint64_t count, \
		 const struct iomap *map), \
	TP_ARGS(inode, pos, count, map))
DEFINE_FUSE_IOMAP_INLINE_EVENT(fuse_iomap_inline_read);
DEFINE_FUSE_IOMAP_INLINE_EVENT(fuse_iomap_inline_write);
DEFINE_FUSE_IOMAP_INLINE_EVENT(fuse_iomap_set_inline_iomap);
DEFINE_FUSE_IOMAP_INLINE_EVENT(fuse_iomap_set_inline_srcmap);

TRACE_EVENT(fuse_iomap_open_truncate,
	TP_PROTO(const struct inode *inode),

	TP_ARGS(inode),

	TP_STRUCT__entry(
		FUSE_INODE_FIELDS
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
	),

	TP_printk(FUSE_INODE_FMT,
		  FUSE_INODE_PRINTK_ARGS)
);

TRACE_EVENT(fuse_iomap_copied_file_range,
	TP_PROTO(const struct inode *inode, loff_t offset,
		 size_t written),
	TP_ARGS(inode, offset, written),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->offset		=	offset;
		__entry->length		=	written;
	),

	TP_printk(FUSE_IO_RANGE_FMT(),
		  FUSE_IO_RANGE_PRINTK_ARGS())
);

DECLARE_EVENT_CLASS(fuse_iext_class,
	TP_PROTO(const struct inode *inode, const struct fuse_iext_cursor *cur,
		 int state, unsigned long caller_ip),

	TP_ARGS(inode, cur, state, caller_ip),

	TP_STRUCT__entry(
		FUSE_INODE_FIELDS
		FUSE_IOMAP_MAP_FIELDS(map)
		__field(void *,			leaf)
		__field(int,			pos)
		__field(int,			iext_state)
		__field(unsigned long,		caller_ip)
	),
	TP_fast_assign(
		const struct fuse_ifork *ifp;
		struct fuse_iomap_io r = { };
		FUSE_INODE_ASSIGN(inode, fi, fm);

		if (state & FUSE_IEXT_WRITE_MAPPING)
			ifp = fi->cache.im_write;
		else
			ifp = &fi->cache.im_read;
		if (ifp)
			fuse_iext_get_extent(ifp, cur, &r);

		__entry->mapoffset	=	r.offset;
		__entry->mapaddr	=	r.addr;
		__entry->maplength	=	r.length;
		__entry->mapdev		=	r.dev;
		__entry->maptype	=	r.type;
		__entry->mapflags	=	r.flags;

		__entry->leaf		=	cur->leaf;
		__entry->pos		=	cur->pos;

		__entry->iext_state	=	state;
		__entry->caller_ip	=	caller_ip;
	),
	TP_printk(FUSE_INODE_FMT " state (%s) cur %p/%d " FUSE_IOMAP_MAP_FMT() " caller %pS",
		  FUSE_INODE_PRINTK_ARGS,
		  __print_flags(__entry->iext_state, "|", FUSE_IEXT_STATE_STRINGS),
		  __entry->leaf,
		  __entry->pos,
		  FUSE_IOMAP_MAP_PRINTK_ARGS(map),
		  (void *)__entry->caller_ip)
)

#define DEFINE_IEXT_EVENT(name) \
DEFINE_EVENT(fuse_iext_class, name, \
	TP_PROTO(const struct inode *inode, const struct fuse_iext_cursor *cur, \
		 int state, unsigned long caller_ip), \
	TP_ARGS(inode, cur, state, caller_ip))
DEFINE_IEXT_EVENT(fuse_iext_insert);
DEFINE_IEXT_EVENT(fuse_iext_remove);
DEFINE_IEXT_EVENT(fuse_iext_pre_update);
DEFINE_IEXT_EVENT(fuse_iext_post_update);

TRACE_EVENT(fuse_iext_update_class,
	TP_PROTO(const struct inode *inode, uint32_t iext_state,
		 const struct fuse_iomap_io *map),
	TP_ARGS(inode, iext_state, map),

	TP_STRUCT__entry(
		FUSE_INODE_FIELDS
		FUSE_IOMAP_MAP_FIELDS(map)
		__field(uint32_t,		iext_state)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->mapoffset	=	map->offset;
		__entry->maplength	=	map->length;
		__entry->maptype	=	map->type;
		__entry->mapflags	=	map->flags;
		__entry->mapdev		=	map->dev;
		__entry->mapaddr	=	map->addr;

		__entry->iext_state	=	iext_state;
	),

	TP_printk(FUSE_INODE_FMT " state (%s)" FUSE_IOMAP_MAP_FMT(),
		  FUSE_INODE_PRINTK_ARGS,
		  __print_flags(__entry->iext_state, "|", FUSE_IEXT_STATE_STRINGS),
		  FUSE_IOMAP_MAP_PRINTK_ARGS(map))
);
#define DEFINE_IEXT_UPDATE_EVENT(name) \
DEFINE_EVENT(fuse_iext_update_class, name, \
	TP_PROTO(const struct inode *inode, uint32_t iext_state, \
		 const struct fuse_iomap_io *map), \
	TP_ARGS(inode, iext_state, map))
DEFINE_IEXT_UPDATE_EVENT(fuse_iext_del_mapping);
DEFINE_IEXT_UPDATE_EVENT(fuse_iext_add_mapping);

TRACE_EVENT(fuse_iext_alt_update_class,
	TP_PROTO(const struct inode *inode, const struct fuse_iomap_io *map),
	TP_ARGS(inode, map),

	TP_STRUCT__entry(
		FUSE_INODE_FIELDS
		FUSE_IOMAP_MAP_FIELDS(map)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);

		__entry->mapoffset	=	map->offset;
		__entry->maplength	=	map->length;
		__entry->maptype	=	map->type;
		__entry->mapflags	=	map->flags;
		__entry->mapdev		=	map->dev;
		__entry->mapaddr	=	map->addr;
	),

	TP_printk(FUSE_INODE_FMT FUSE_IOMAP_MAP_FMT(),
		  FUSE_INODE_PRINTK_ARGS,
		  FUSE_IOMAP_MAP_PRINTK_ARGS(map))
);
#define DEFINE_IEXT_ALT_UPDATE_EVENT(name) \
DEFINE_EVENT(fuse_iext_alt_update_class, name, \
	TP_PROTO(const struct inode *inode, const struct fuse_iomap_io *map), \
	TP_ARGS(inode, map))
DEFINE_IEXT_ALT_UPDATE_EVENT(fuse_iext_del_mapping_got);
DEFINE_IEXT_ALT_UPDATE_EVENT(fuse_iext_add_mapping_left);
DEFINE_IEXT_ALT_UPDATE_EVENT(fuse_iext_add_mapping_right);

TRACE_EVENT(fuse_iomap_cache_remove,
	TP_PROTO(const struct inode *inode, enum fuse_iomap_iodir iodir,
		 loff_t offset, uint64_t length, unsigned long caller_ip),
	TP_ARGS(inode, iodir, offset, length, caller_ip),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		FUSE_IOMAP_IODIR_FIELD
		__field(unsigned long,		caller_ip)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->iodir		=	iodir;
		__entry->offset		=	offset;
		__entry->length		=	length;
		__entry->caller_ip	=	caller_ip;
	),

	TP_printk(FUSE_IO_RANGE_FMT() FUSE_IOMAP_IODIR_FMT " caller %pS",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  FUSE_IOMAP_IODIR_PRINTK_ARGS,
		  (void *)__entry->caller_ip)
);

TRACE_EVENT(fuse_iomap_cached_mapping_class,
	TP_PROTO(const struct inode *inode, enum fuse_iomap_iodir iodir,
		 const struct fuse_iomap_io *map, unsigned long caller_ip),
	TP_ARGS(inode, iodir, map, caller_ip),

	TP_STRUCT__entry(
		FUSE_INODE_FIELDS
		FUSE_IOMAP_IODIR_FIELD
		FUSE_IOMAP_MAP_FIELDS(map)
		__field(unsigned long,		caller_ip)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->iodir		=	iodir;

		__entry->mapoffset	=	map->offset;
		__entry->maplength	=	map->length;
		__entry->maptype	=	map->type;
		__entry->mapflags	=	map->flags;
		__entry->mapdev		=	map->dev;
		__entry->mapaddr	=	map->addr;

		__entry->caller_ip	=	caller_ip;
	),

	TP_printk(FUSE_INODE_FMT FUSE_IOMAP_IODIR_FMT FUSE_IOMAP_MAP_FMT() " caller %pS",
		  FUSE_INODE_PRINTK_ARGS,
		  FUSE_IOMAP_IODIR_PRINTK_ARGS,
		  FUSE_IOMAP_MAP_PRINTK_ARGS(map),
		  (void *)__entry->caller_ip)
);
#define DEFINE_FUSE_IOMAP_CACHED_MAPPING_EVENT(name) \
DEFINE_EVENT(fuse_iomap_cached_mapping_class, name, \
	TP_PROTO(const struct inode *inode, enum fuse_iomap_iodir iodir, \
		 const struct fuse_iomap_io *map, unsigned long caller_ip), \
	TP_ARGS(inode, iodir, map, caller_ip))
DEFINE_FUSE_IOMAP_CACHED_MAPPING_EVENT(fuse_iomap_cache_add);
DEFINE_FUSE_IOMAP_CACHED_MAPPING_EVENT(fuse_iext_check_mapping);

TRACE_EVENT(fuse_iomap_cache_lookup,
	TP_PROTO(const struct inode *inode, enum fuse_iomap_iodir iodir,
		 loff_t pos, uint64_t count, unsigned long caller_ip),
	TP_ARGS(inode, iodir, pos, count, caller_ip),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()
		FUSE_IOMAP_IODIR_FIELD
		__field(unsigned long,		caller_ip)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->iodir		=	iodir;
		__entry->offset		=	pos;
		__entry->length		=	count;
		__entry->caller_ip	=	caller_ip;
	),

	TP_printk(FUSE_IO_RANGE_FMT() FUSE_IOMAP_IODIR_FMT " caller %pS",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  FUSE_IOMAP_IODIR_PRINTK_ARGS,
		  (void *)__entry->caller_ip)
);

TRACE_EVENT(fuse_iomap_cache_lookup_result,
	TP_PROTO(const struct inode *inode, enum fuse_iomap_iodir iodir,
		 loff_t pos, uint64_t count, const struct fuse_iomap_io *got,
		 const struct fuse_iomap_lookup *map),
	TP_ARGS(inode, iodir, pos, count, got, map),

	TP_STRUCT__entry(
		FUSE_IO_RANGE_FIELDS()

		FUSE_IOMAP_MAP_FIELDS(got)
		FUSE_IOMAP_MAP_FIELDS(map)

		FUSE_IOMAP_IODIR_FIELD
		__field(uint64_t,		validity_cookie)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);
		__entry->iodir		=	iodir;
		__entry->offset		=	pos;
		__entry->length		=	count;

		__entry->gotoffset	=	got->offset;
		__entry->gotlength	=	got->length;
		__entry->gottype	=	got->type;
		__entry->gotflags	=	got->flags;
		__entry->gotdev		=	got->dev;
		__entry->gotaddr	=	got->addr;

		__entry->mapoffset	=	map->map.offset;
		__entry->maplength	=	map->map.length;
		__entry->maptype	=	map->map.type;
		__entry->mapflags	=	map->map.flags;
		__entry->mapdev		=	map->map.dev;
		__entry->mapaddr	=	map->map.addr;

		__entry->validity_cookie=	map->validity_cookie;
	),

	TP_printk(FUSE_IO_RANGE_FMT() FUSE_IOMAP_IODIR_FMT FUSE_IOMAP_MAP_FMT("map") FUSE_IOMAP_MAP_FMT("got") " cookie 0x%llx",
		  FUSE_IO_RANGE_PRINTK_ARGS(),
		  FUSE_IOMAP_IODIR_PRINTK_ARGS,
		  FUSE_IOMAP_MAP_PRINTK_ARGS(map),
		  FUSE_IOMAP_MAP_PRINTK_ARGS(got),
		  __entry->validity_cookie)
);

TRACE_EVENT(fuse_iomap_invalid,
	TP_PROTO(const struct inode *inode, const struct iomap *map,
		 uint64_t validity_cookie),
	TP_ARGS(inode, map, validity_cookie),

	TP_STRUCT__entry(
		FUSE_INODE_FIELDS
		FUSE_IOMAP_MAP_FIELDS(map)
		__field(uint64_t,		old_validity_cookie)
		__field(uint64_t,		validity_cookie)
	),

	TP_fast_assign(
		FUSE_INODE_ASSIGN(inode, fi, fm);

		__entry->mapoffset	=	map->offset;
		__entry->maplength	=	map->length;
		__entry->maptype	=	map->type;
		__entry->mapflags	=	map->flags;
		__entry->mapaddr	=	map->addr;
		__entry->mapdev		=	FUSE_IOMAP_DEV_NULL;

		__entry->old_validity_cookie=	map->validity_cookie;
		__entry->validity_cookie=	validity_cookie;
	),

	TP_printk(FUSE_INODE_FMT FUSE_IOMAP_MAP_FMT() " old_cookie 0x%llx new_cookie 0x%llx",
		  FUSE_INODE_PRINTK_ARGS,
		  FUSE_IOMAP_MAP_PRINTK_ARGS(map),
		  __entry->old_validity_cookie,
		  __entry->validity_cookie)
);
#endif /* CONFIG_FUSE_IOMAP */

#endif /* _TRACE_FUSE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE fuse_trace
#include <trace/define_trace.h>
