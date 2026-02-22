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
	EM( FUSE_IOMAP_BEGIN,		"FUSE_IOMAP_BEGIN")	\
	EM( FUSE_IOMAP_END,		"FUSE_IOMAP_END")	\
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
		const struct fuse_inode *fi = get_fuse_inode(inode); \
		const struct fuse_mount *fm = get_fuse_mount(inode); \
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

#ifdef CONFIG_FUSE_BACKING
#define FUSE_BACKING_FLAG_STRINGS \
	{ FUSE_BACKING_TYPE_PASSTHROUGH,	"pass" }, \
	{ FUSE_BACKING_TYPE_IOMAP,		"iomap" }

DECLARE_EVENT_CLASS(fuse_backing_class,
	TP_PROTO(const struct fuse_conn *fc, const struct fuse_backing *fb),

	TP_ARGS(fc, fb),

	TP_STRUCT__entry(
		__field(dev_t,			connection)
		__field(int,			id)
		__field(unsigned int,		type)
		__field(unsigned long,		ino)
		__field(dev_t,			rdev)
	),

	TP_fast_assign(
		struct inode *inode = file_inode(fb->file);

		__entry->connection	=	fc->dev;
		__entry->id		=	fb->id;
		__entry->ino		=	inode->i_ino;
		__entry->type		=	fb->ops->type;
		if (fb->ops->type == FUSE_BACKING_TYPE_IOMAP)
			__entry->rdev	=	inode->i_rdev;
		else
			__entry->rdev	=	0;
	),

	TP_printk("connection %u id %d type %s ino 0x%lx rdev %u:%u",
		  __entry->connection,
		  __entry->id,
		  __print_symbolic(__entry->type, FUSE_BACKING_FLAG_STRINGS),
		  __entry->ino,
		  MAJOR(__entry->rdev), MINOR(__entry->rdev))
);
#define DEFINE_FUSE_BACKING_EVENT(name)		\
DEFINE_EVENT(fuse_backing_class, name,		\
	TP_PROTO(const struct fuse_conn *fc, const struct fuse_backing *fb), \
	TP_ARGS(fc, fb))
DEFINE_FUSE_BACKING_EVENT(fuse_backing_open);
DEFINE_FUSE_BACKING_EVENT(fuse_backing_close);
#endif /* CONFIG_FUSE_BACKING */

#if IS_ENABLED(CONFIG_FUSE_IOMAP)

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
	{ FUSE_IOMAP_OP_DONTCACHE,		"dontcache" }

#define FUSE_IOMAP_TYPE_STRINGS \
	{ FUSE_IOMAP_TYPE_PURE_OVERWRITE,	"overwrite" }, \
	{ FUSE_IOMAP_TYPE_HOLE,			"hole" }, \
	{ FUSE_IOMAP_TYPE_DELALLOC,		"delalloc" }, \
	{ FUSE_IOMAP_TYPE_MAPPED,		"mapped" }, \
	{ FUSE_IOMAP_TYPE_UNWRITTEN,		"unwritten" }, \
	{ FUSE_IOMAP_TYPE_INLINE,		"inline" }

TRACE_DEFINE_ENUM(FUSE_I_ADVISE_RDPLUS);
TRACE_DEFINE_ENUM(FUSE_I_INIT_RDPLUS);
TRACE_DEFINE_ENUM(FUSE_I_SIZE_UNSTABLE);
TRACE_DEFINE_ENUM(FUSE_I_BAD);
TRACE_DEFINE_ENUM(FUSE_I_BTIME);
TRACE_DEFINE_ENUM(FUSE_I_CACHE_IO_MODE);
TRACE_DEFINE_ENUM(FUSE_I_EXCLUSIVE);
TRACE_DEFINE_ENUM(FUSE_I_IOMAP);

#define FUSE_IFLAG_STRINGS \
	{ 1 << FUSE_I_ADVISE_RDPLUS,		"advise_rdplus" }, \
	{ 1 << FUSE_I_INIT_RDPLUS,		"init_rdplus" }, \
	{ 1 << FUSE_I_SIZE_UNSTABLE,		"size_unstable" }, \
	{ 1 << FUSE_I_BAD,			"bad" }, \
	{ 1 << FUSE_I_BTIME,			"btime" }, \
	{ 1 << FUSE_I_CACHE_IO_MODE,		"cacheio" }, \
	{ 1 << FUSE_I_EXCLUSIVE,		"excl" }, \
	{ 1 << FUSE_I_IOMAP,			"iomap" }

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
#endif /* CONFIG_FUSE_IOMAP */

#endif /* _TRACE_FUSE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE fuse_trace
#include <trace/define_trace.h>
