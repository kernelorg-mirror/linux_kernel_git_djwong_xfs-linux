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

#if IS_ENABLED(CONFIG_FUSE_IOMAP)
struct fuse_iext_cursor;

#define FUSE_IOMAP_F_STRINGS \
	{ FUSE_IOMAP_F_NEW,			"new" }, \
	{ FUSE_IOMAP_F_DIRTY,			"dirty" }, \
	{ FUSE_IOMAP_F_SHARED,			"shared" }, \
	{ FUSE_IOMAP_F_MERGED,			"merged" }, \
	{ FUSE_IOMAP_F_XATTR,			"xattr" }, \
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
	{ FUSE_IOMAP_OP_ATOMIC,			"atomic" }, \
	{ FUSE_IOMAP_OP_DONTCACHE,		"dontcache" }

#define FUSE_IOMAP_TYPE_STRINGS \
	{ FUSE_IOMAP_TYPE_PURE_OVERWRITE,	"overwrite" }, \
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
	{ FUSE_IOMAP_IOEND_APPEND,		"append" }

#define IOMAP_DIOEND_STRINGS \
	{ IOMAP_DIO_UNWRITTEN,			"unwritten" }, \
	{ IOMAP_DIO_COW,			"cow" }

#define IOMAP_IOEND_STRINGS \
	{ IOMAP_IOEND_SHARED,			"shared" }, \
	{ IOMAP_IOEND_UNWRITTEN,		"unwritten" }, \
	{ IOMAP_IOEND_BOUNDARY,			"boundary" }, \
	{ IOMAP_IOEND_DIRECT,			"direct" }

TRACE_DEFINE_ENUM(FUSE_IOMAP_READ_FORK);
TRACE_DEFINE_ENUM(FUSE_IOMAP_WRITE_FORK);

#define FUSE_IOMAP_FORK_STRINGS \
	{ FUSE_IOMAP_READ_FORK,			"read" }, \
	{ FUSE_IOMAP_WRITE_FORK,		"write" }

#define FUSE_IOMAP_CACHE_LOCK_STRINGS \
	{ FUSE_IOMAP_LOCK_SHARED,		"shared" }, \
	{ FUSE_IOMAP_LOCK_EXCL,			"exclusive" }

#define FUSE_IEXT_STATE_STRINGS \
	{ FUSE_IEXT_LEFT_CONTIG,		"l_cont" }, \
	{ FUSE_IEXT_RIGHT_CONTIG,		"r_cont" }, \
	{ FUSE_IEXT_LEFT_FILLING,		"l_fill" }, \
	{ FUSE_IEXT_RIGHT_FILLING,		"r_fill" }, \
	{ FUSE_IEXT_LEFT_VALID,			"l_valid" }, \
	{ FUSE_IEXT_RIGHT_VALID,		"r_valid" }, \
	{ FUSE_IEXT_WRITEFORK,			"writefork" }

TRACE_EVENT(fuse_iomap_begin,
	TP_PROTO(const struct inode *inode, loff_t pos, loff_t count,
		 unsigned opflags),

	TP_ARGS(inode, pos, count, opflags),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		pos)
		__field(loff_t,		count)
		__field(unsigned,	opflags)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->pos		=	pos;
		__entry->count		=	count;
		__entry->opflags	=	opflags;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx opflags (%s) pos 0x%llx count 0x%llx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_flags(__entry->opflags, "|", FUSE_IOMAP_OP_STRINGS),
		  __entry->pos, __entry->count)
);

TRACE_EVENT(fuse_iomap_begin_error,
	TP_PROTO(const struct inode *inode, loff_t pos, loff_t count,
		 unsigned opflags, int error),

	TP_ARGS(inode, pos, count, opflags, error),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		pos)
		__field(loff_t,		count)
		__field(unsigned,	opflags)
		__field(int,		error)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->pos		=	pos;
		__entry->count		=	count;
		__entry->opflags	=	opflags;
		__entry->error		=	error;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx opflags (%s) pos 0x%llx count 0x%llx err %d",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_flags(__entry->opflags, "|", FUSE_IOMAP_OP_STRINGS),
		  __entry->pos, __entry->count, __entry->error)
);

TRACE_EVENT(fuse_iomap_read_map,
	TP_PROTO(const struct inode *inode,
		 const struct fuse_iomap_begin_out *outarg),

	TP_ARGS(inode, outarg),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		offset)
		__field(loff_t,		length)
		__field(uint32_t,	dev)
		__field(uint64_t,	addr)
		__field(uint16_t,	type)
		__field(uint16_t,	mapflags)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->offset		=	outarg->offset;
		__entry->length		=	outarg->length;
		__entry->dev		=	outarg->read_dev;
		__entry->addr		=	outarg->read_addr;
		__entry->type		=	outarg->read_type;
		__entry->mapflags	=	outarg->read_flags;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx read offset 0x%llx count 0x%llx dev %u addr 0x%llx type %s mapflags (%s)",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->offset, __entry->length,
		  __entry->dev, __entry->addr,
		  __print_symbolic(__entry->type, FUSE_IOMAP_TYPE_STRINGS),
		  __print_flags(__entry->mapflags, "|", FUSE_IOMAP_F_STRINGS))
);

TRACE_EVENT(fuse_iomap_write_map,
	TP_PROTO(const struct inode *inode,
		 const struct fuse_iomap_begin_out *outarg),

	TP_ARGS(inode, outarg),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		offset)
		__field(loff_t,		length)
		__field(uint32_t,	dev)
		__field(uint64_t,	addr)
		__field(uint16_t,	type)
		__field(uint16_t,	mapflags)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->offset		=	outarg->offset;
		__entry->length		=	outarg->length;
		__entry->dev		=	outarg->write_dev;
		__entry->addr		=	outarg->write_addr;
		__entry->type		=	outarg->write_type;
		__entry->mapflags	=	outarg->write_flags;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx write offset 0x%llx count 0x%llx dev %u addr 0x%llx type %s mapflags (%s)",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->offset, __entry->length,
		  __entry->dev, __entry->addr,
		  __print_symbolic(__entry->type, FUSE_IOMAP_TYPE_STRINGS),
		  __print_flags(__entry->mapflags, "|", FUSE_IOMAP_F_STRINGS))
);

TRACE_EVENT(fuse_iomap_end,
	TP_PROTO(const struct inode *inode,
		 const struct fuse_iomap_end_in *inarg),

	TP_ARGS(inode, inarg),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		pos)
		__field(loff_t,		count)
		__field(unsigned,	opflags)
		__field(size_t,		written)

		__field(uint32_t,	dev)
		__field(uint64_t,	addr)
		__field(uint16_t,	type)
		__field(uint16_t,	mapflags)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->pos		=	inarg->pos;
		__entry->count		=	inarg->count;
		__entry->opflags	=	inarg->opflags;
		__entry->written	=	inarg->written;
		__entry->dev		=	inarg->map_dev;
		__entry->addr		=	inarg->map_addr;
		__entry->type		=	inarg->map_type;
		__entry->mapflags	=	inarg->map_flags;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx opflags (%s) pos 0x%llx count 0x%llx written %zd dev %u addr 0x%llx type 0x%x mapflags (%s)",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_flags(__entry->opflags, "|", FUSE_IOMAP_OP_STRINGS),
		  __entry->pos, __entry->count, __entry->written, __entry->dev,
		  __entry->addr, __entry->type,
		  __print_flags(__entry->mapflags, "|", FUSE_IOMAP_F_STRINGS))
);

TRACE_EVENT(fuse_iomap_end_error,
	TP_PROTO(const struct inode *inode,
		 const struct fuse_iomap_end_in *inarg, int error),

	TP_ARGS(inode, inarg, error),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		pos)
		__field(loff_t,		count)
		__field(unsigned,	opflags)
		__field(size_t,		written)
		__field(int,		error)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->pos		=	inarg->pos;
		__entry->count		=	inarg->count;
		__entry->opflags	=	inarg->opflags;
		__entry->written	=	inarg->written;
		__entry->error		=	error;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx opflags (%s) pos 0x%llx count 0x%llx written %zd error %d",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_flags(__entry->opflags, "|", FUSE_IOMAP_OP_STRINGS),
		  __entry->pos, __entry->count, __entry->written,
		  __entry->error)
);

TRACE_EVENT(fuse_iomap_ioend,
	TP_PROTO(const struct inode *inode,
		 const struct fuse_iomap_ioend_in *inarg),

	TP_ARGS(inode, inarg),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(unsigned,	ioendflags)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		pos)
		__field(int,		error)
		__field(uint64_t,	new_addr)
		__field(size_t,		written)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->ioendflags	=	inarg->ioendflags;
		__entry->error		=	inarg->error;
		__entry->pos		=	inarg->pos;
		__entry->new_addr	=	inarg->new_addr;
		__entry->written	=	inarg->written;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx ioendflags (%s) pos 0x%llx written %zd error %d new_addr 0x%llx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_flags(__entry->ioendflags, "|", FUSE_IOMAP_IOEND_STRINGS),
		  __entry->pos, __entry->written, __entry->error,
		  __entry->new_addr)
);

TRACE_EVENT(fuse_iomap_ioend_error,
	TP_PROTO(const struct inode *inode,
		 const struct fuse_iomap_ioend_in *inarg,
		 int error),

	TP_ARGS(inode, inarg, error),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(unsigned,	ioendflags)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		pos)
		__field(int,		error)
		__field(uint64_t,	new_addr)
		__field(size_t,		written)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->ioendflags	=	inarg->ioendflags;
		__entry->error		=	error;
		__entry->pos		=	inarg->pos;
		__entry->new_addr	=	inarg->new_addr;
		__entry->written	=	inarg->written;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx ioendflags (%s) pos 0x%llx written %zd error %d new_addr 0x%llx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_flags(__entry->ioendflags, "|", FUSE_IOMAP_IOEND_STRINGS),
		  __entry->pos, __entry->written, __entry->error,
		  __entry->new_addr)
);

TRACE_EVENT(fuse_iomap_dev_add,
	TP_PROTO(const struct fuse_conn *fc,
		 const struct fuse_backing_map *map),

	TP_ARGS(fc, map),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(int,		fd)
		__field(unsigned int,	flags)
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

TRACE_EVENT(fuse_iomap_dev_class,
	TP_PROTO(const struct fuse_conn *fc, unsigned int idx,
		 const struct fuse_iomap_dev *fb),

	TP_ARGS(fc, idx, fb),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(unsigned int,	idx)
		__field(dev_t,		bdev)
	),

	TP_fast_assign(
		__entry->connection	=	fc->dev;
		__entry->idx		=	idx;

		if (fb) {
			struct inode *inode = file_inode(fb->file);

			__entry->bdev	=	inode->i_rdev;
		} else {
			__entry->bdev	=	0;
		}
	),

	TP_printk("connection %u idx %u dev %u:%u",
		  __entry->connection,
		  __entry->idx,
		  MAJOR(__entry->bdev), MINOR(__entry->bdev))
);
#define DEFINE_FUSE_IOMAP_DEV_EVENT(name)		\
DEFINE_EVENT(fuse_iomap_dev_class, name,		\
	TP_PROTO(const struct fuse_conn *fc, unsigned int idx, \
		 const struct fuse_iomap_dev *fb), \
	TP_ARGS(fc, idx, fb))
DEFINE_FUSE_IOMAP_DEV_EVENT(fuse_iomap_add_dev);
DEFINE_FUSE_IOMAP_DEV_EVENT(fuse_iomap_remove_dev);

TRACE_EVENT(fuse_iomap_fiemap,
	TP_PROTO(const struct inode *inode, u64 start, u64 count,
		unsigned int flags),

	TP_ARGS(inode, start, count, flags),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(u64,		start)
		__field(u64,		count)
		__field(unsigned int,	flags)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->start		=	start;
		__entry->count		=	count;
		__entry->flags		=	flags;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx flags 0x%x start 0x%llx count 0x%llx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->flags, __entry->start,
		  __entry->count)
);

TRACE_EVENT(fuse_iomap_lseek,
	TP_PROTO(const struct inode *inode, loff_t offset, int whence),

	TP_ARGS(inode, offset, whence),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		offset)
		__field(int,		whence)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->offset		=	offset;
		__entry->whence		=	whence;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx offset 0x%llx whence %d",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->offset, __entry->whence)
);

DECLARE_EVENT_CLASS(fuse_iomap_file_io_class,
	TP_PROTO(const struct kiocb *iocb, const struct iov_iter *iter),
	TP_ARGS(iocb, iter),
	TP_STRUCT__entry(
		__field(dev_t, connection)
		__field(uint64_t, ino)
		__field(uint64_t,	nodeid)
		__field(loff_t, isize)
		__field(loff_t, offset)
		__field(size_t, count)
	),
	TP_fast_assign(
		const struct inode *inode = file_inode(iocb->ki_filp);
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->offset		=	iocb->ki_pos;
		__entry->count		=	iov_iter_count(iter);
	),
	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx pos 0x%llx bytecount 0x%zx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->offset, __entry->count)
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
		__field(dev_t, connection)
		__field(uint64_t, ino)
		__field(uint64_t, nodeid)
		__field(loff_t, isize)
		__field(loff_t, offset)
		__field(size_t, count)
		__field(ssize_t, ret)
	),
	TP_fast_assign(
		const struct inode *inode = file_inode(iocb->ki_filp);
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->offset		=	iocb->ki_pos;
		__entry->count		=	iov_iter_count(iter);
		__entry->ret		=	ret;
	),
	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx pos 0x%llx bytecount 0x%zx ret 0x%zx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->offset, __entry->count, __entry->ret)
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
		__field(dev_t,		connection)
		__field(unsigned,	dioendflags)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		pos)
		__field(size_t,		written)
		__field(int,		error)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->dioendflags	=	flags;
		__entry->error		=	error;
		__entry->pos		=	pos;
		__entry->written	=	written;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx dioendflags (%s) pos 0x%llx written %zd error %d",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_flags(__entry->dioendflags, "|", IOMAP_DIOEND_STRINGS),
		  __entry->pos, __entry->written, __entry->error)
);

TRACE_EVENT(fuse_iomap_end_ioend,
	TP_PROTO(const struct iomap_ioend *ioend),

	TP_ARGS(ioend),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		offset)
		__field(size_t,		size)
		__field(unsigned int,	ioendflags)
		__field(int,		error)
	),

	TP_fast_assign(
		const struct inode *inode = ioend->io_inode;
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->offset		=	ioend->io_offset;
		__entry->size		=	ioend->io_size;
		__entry->ioendflags	=	ioend->io_flags;
		__entry->error		=
				blk_status_to_errno(ioend->io_bio.bi_status);
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx offset 0x%llx size %zu ioendflags (%s) error %d",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->offset, __entry->size,
		  __print_flags(__entry->ioendflags, "|", IOMAP_IOEND_STRINGS),
		  __entry->error)
);

TRACE_EVENT(fuse_iomap_map_blocks,
	TP_PROTO(const struct inode *inode, loff_t offset, unsigned int count),

	TP_ARGS(inode, offset, count),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		offset)
		__field(unsigned int,	count)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->offset		=	offset;
		__entry->count		=	count;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx offset 0x%llx count %u",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->offset, __entry->count)
);

TRACE_EVENT(fuse_iomap_submit_ioend,
	TP_PROTO(const struct inode *inode, unsigned int nr_folios, int error),

	TP_ARGS(inode, nr_folios, error),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(unsigned int,	nr_folios)
		__field(int,		error)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->nr_folios	=	nr_folios;
		__entry->error		=	error;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx nr_folios %u error %d",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->nr_folios, __entry->error)
);

TRACE_EVENT(fuse_iomap_discard_folio,
	TP_PROTO(const struct inode *inode, loff_t offset, size_t count),

	TP_ARGS(inode, offset, count),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		offset)
		__field(size_t,		count)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->offset		=	offset;
		__entry->count		=	count;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx offset 0x%llx count 0x%zx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->offset, __entry->count)
);

TRACE_EVENT(fuse_iomap_writepages,
	TP_PROTO(const struct inode *inode, const struct writeback_control *wbc),

	TP_ARGS(inode, wbc),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		start)
		__field(loff_t,		end)
		__field(long,		nr_to_write)
		__field(bool,		sync_all)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->start		=	wbc->range_start;
		__entry->end		=	wbc->range_end;
		__entry->nr_to_write	=	wbc->nr_to_write;
		__entry->sync_all	=	wbc->sync_mode == WB_SYNC_ALL;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx start 0x%llx end 0x%llx nr %ld sync_all? %d",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->start, __entry->end,
		  __entry->nr_to_write, __entry->sync_all)
);

TRACE_EVENT(fuse_iomap_read_folio,
	TP_PROTO(const struct folio *folio),

	TP_ARGS(folio),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		pos)
		__field(size_t,		count)
	),

	TP_fast_assign(
		const struct inode *inode = folio->mapping->host;
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->pos		=	folio_pos(folio);
		__entry->count		=	folio_size(folio);
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx offset 0x%llx count 0x%zx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->pos, __entry->count)
);

TRACE_EVENT(fuse_iomap_readahead,
	TP_PROTO(const struct readahead_control *rac),

	TP_ARGS(rac),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		pos)
		__field(size_t,		count)
	),

	TP_fast_assign(
		const struct inode *inode = file_inode(rac->file);
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);
		struct readahead_control *mutrac = (struct readahead_control *)rac;

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->pos		=	readahead_pos(mutrac);
		__entry->count		=	readahead_length(mutrac);
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx offset 0x%llx count 0x%zx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->pos, __entry->count)
);

TRACE_EVENT(fuse_iomap_page_mkwrite,
	TP_PROTO(const struct vm_fault *vmf),

	TP_ARGS(vmf),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		pos)
		__field(size_t,		count)
	),

	TP_fast_assign(
		const struct inode *inode = file_inode(vmf->vma->vm_file);
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);
		struct folio *folio = page_folio(vmf->page);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->pos		=	folio_pos(folio);
		__entry->count		=	folio_size(folio);
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx offset 0x%llx count 0x%zx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->pos, __entry->count)
);

DECLARE_EVENT_CLASS(fuse_iomap_file_range_class,
	TP_PROTO(const struct inode *inode, loff_t offset, loff_t length),
	TP_ARGS(inode, offset, length),
	TP_STRUCT__entry(
		__field(dev_t, connection)
		__field(uint64_t, ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t, offset)
		__field(loff_t, length)
	),
	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->offset		=	offset;
		__entry->length		=	length;
	),
	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx pos 0x%llx bytecount 0x%llx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->offset, __entry->length)
)
#define DEFINE_FUSE_IOMAP_FILE_RANGE_EVENT(name)		\
DEFINE_EVENT(fuse_iomap_file_range_class, name,		\
	TP_PROTO(const struct inode *inode, loff_t offset, loff_t length), \
	TP_ARGS(inode, offset, length))
DEFINE_FUSE_IOMAP_FILE_RANGE_EVENT(fuse_iomap_truncate_up);
DEFINE_FUSE_IOMAP_FILE_RANGE_EVENT(fuse_iomap_truncate_down);
DEFINE_FUSE_IOMAP_FILE_RANGE_EVENT(fuse_iomap_punch_range);
DEFINE_FUSE_IOMAP_FILE_RANGE_EVENT(fuse_iomap_setsize);

TRACE_EVENT(fuse_iomap_set_i_blkbits,
	TP_PROTO(const struct inode *inode, u8 new_blkbits),
	TP_ARGS(inode, new_blkbits),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(u8,		old_blkbits)
		__field(u8,		new_blkbits)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->old_blkbits	=	inode->i_blkbits;
		__entry->new_blkbits	=	new_blkbits;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx old_blkbits %u new_blkbits %u",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->old_blkbits, __entry->new_blkbits)
);

TRACE_EVENT(fuse_iomap_fallocate,
	TP_PROTO(const struct inode *inode, int mode, loff_t offset,
		 loff_t length, loff_t newsize),
	TP_ARGS(inode, mode, offset, length, newsize),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(loff_t,		offset)
		__field(loff_t,		length)
		__field(loff_t,		newsize)
		__field(int,		mode)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->mode		=	mode;
		__entry->offset		=	offset;
		__entry->length		=	length;
		__entry->newsize	=	newsize;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx mode 0x%x offset 0x%llx length 0x%llx newsize 0x%llx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize, __entry->mode, __entry->offset,
		  __entry->length, __entry->newsize)
);

DECLARE_EVENT_CLASS(fuse_iomap_cache_lock_class,
	TP_PROTO(const struct inode *inode, unsigned int lock_flags,
		 unsigned long caller_ip),
	TP_ARGS(inode, lock_flags, caller_ip),
	TP_STRUCT__entry(
		__field(dev_t, connection)
		__field(uint64_t, ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(unsigned int, lock_flags)
		__field(unsigned long, caller_ip)
	),
	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->lock_flags	=	lock_flags;
		__entry->caller_ip	=	caller_ip;
	),
	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx lock (%s) caller %pS",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_flags(__entry->lock_flags, "|", FUSE_IOMAP_CACHE_LOCK_STRINGS),
		  (void *)__entry->caller_ip)
)
#define DEFINE_FUSE_IOMAP_CACHE_LOCK_EVENT(name)	\
DEFINE_EVENT(fuse_iomap_cache_lock_class, name,		\
	TP_PROTO(const struct inode *inode, unsigned int lock_flags, \
		 unsigned long caller_ip), \
	TP_ARGS(inode, lock_flags, caller_ip))
DEFINE_FUSE_IOMAP_CACHE_LOCK_EVENT(fuse_iomap_cache_lock);
DEFINE_FUSE_IOMAP_CACHE_LOCK_EVENT(fuse_iomap_cache_unlock);

DECLARE_EVENT_CLASS(fuse_iext_class,
	TP_PROTO(const struct inode *inode, const struct fuse_iext_cursor *cur,
		 int state, unsigned long caller_ip),

	TP_ARGS(inode, cur, state, caller_ip),

	TP_STRUCT__entry(
		__field(dev_t, connection)
		__field(uint64_t, ino)
		__field(void *, leaf)
		__field(int, pos)
		__field(loff_t, offset)
		__field(uint64_t, addr)
		__field(uint64_t, length)
		__field(uint16_t, type)
		__field(uint16_t, mapflags)
		__field(uint32_t, dev)
		__field(int, iext_state)
		__field(unsigned long, caller_ip)
	),
	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);
		const struct fuse_ifork *ifp;
		struct fuse_iomap	r = { };

		if (state & FUSE_IEXT_WRITEFORK)
			ifp = fi->cache.im_write;
		else
			ifp = &fi->cache.im_read;
		if (ifp)
			fuse_iext_get_extent(ifp, cur, &r);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->leaf		=	cur->leaf;
		__entry->pos		=	cur->pos;
		__entry->offset		=	r.offset;
		__entry->addr		=	r.addr;
		__entry->length		=	r.length;
		__entry->dev		=	r.dev;
		__entry->type		=	r.type;
		__entry->mapflags	=	r.flags;
		__entry->iext_state	=	state;
		__entry->caller_ip	=	caller_ip;
	),
	TP_printk("connection %u ino %llu state (%s) cur %p/%d "
		  "offset 0x%llx addr 0x%llx length 0x%llx type %s mapflags (%s) dev %u caller %pS",
		  __entry->connection, __entry->ino,
		  __print_flags(__entry->iext_state, "|", FUSE_IEXT_STATE_STRINGS),
		  __entry->leaf,
		  __entry->pos,
		  __entry->offset,
		  __entry->addr,
		  __entry->length,
		  __print_symbolic(__entry->type, FUSE_IOMAP_TYPE_STRINGS),
		  __print_flags(__entry->mapflags, "|", FUSE_IOMAP_F_STRINGS),
		  __entry->dev,
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
		 const struct fuse_iomap *map),
	TP_ARGS(inode, iext_state, map),

	TP_STRUCT__entry(
		__field(dev_t,			connection)
		__field(uint64_t,		ino)
		__field(uint64_t,		nodeid)
		__field(loff_t,			isize)

		__field(loff_t,			map_offset)
		__field(loff_t,			map_length)
		__field(uint16_t,		map_type)
		__field(uint16_t,		map_flags)
		__field(uint32_t,		map_dev)
		__field(uint64_t,		map_addr)

		__field(uint32_t,		iext_state)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);

		__entry->map_offset	=	map->offset;
		__entry->map_length	=	map->length;
		__entry->map_type	=	map->type;
		__entry->map_flags	=	map->flags;
		__entry->map_dev	=	map->dev;
		__entry->map_addr	=	map->addr;

		__entry->iext_state	=	iext_state;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx state (%s) offset 0x%llx length 0x%llx type %s mapflags (%s) dev %u addr 0x%llx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_flags(__entry->iext_state, "|", FUSE_IEXT_STATE_STRINGS),
		  __entry->map_offset, __entry->map_length,
		  __print_symbolic(__entry->map_type, FUSE_IOMAP_TYPE_STRINGS),
		  __print_flags(__entry->map_flags, "|", FUSE_IOMAP_F_STRINGS),
		  __entry->map_dev, __entry->map_addr)
);
#define DEFINE_IEXT_UPDATE_EVENT(name) \
DEFINE_EVENT(fuse_iext_update_class, name, \
	TP_PROTO(const struct inode *inode, uint32_t iext_state, \
		 const struct fuse_iomap *map), \
	TP_ARGS(inode, iext_state, map))
DEFINE_IEXT_UPDATE_EVENT(fuse_iext_del_mapping);
DEFINE_IEXT_UPDATE_EVENT(fuse_iext_add_mapping);

TRACE_EVENT(fuse_iext_alt_update_class,
	TP_PROTO(const struct inode *inode, const struct fuse_iomap *map),
	TP_ARGS(inode, map),

	TP_STRUCT__entry(
		__field(dev_t,			connection)
		__field(uint64_t,		ino)
		__field(uint64_t,		nodeid)

		__field(loff_t,			map_offset)
		__field(loff_t,			map_length)
		__field(uint16_t,		map_type)
		__field(uint16_t,		map_flags)
		__field(uint32_t,		map_dev)
		__field(uint64_t,		map_addr)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;

		__entry->map_offset	=	map->offset;
		__entry->map_length	=	map->length;
		__entry->map_type	=	map->type;
		__entry->map_flags	=	map->flags;
		__entry->map_dev	=	map->dev;
		__entry->map_addr	=	map->addr;
	),

	TP_printk("connection %u ino %llu nodeid %llu offset 0x%llx length 0x%llx type %s mapflags (%s) dev %u addr 0x%llx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->map_offset, __entry->map_length,
		  __print_symbolic(__entry->map_type, FUSE_IOMAP_TYPE_STRINGS),
		  __print_flags(__entry->map_flags, "|", FUSE_IOMAP_F_STRINGS),
		  __entry->map_dev, __entry->map_addr)
);
#define DEFINE_IEXT_ALT_UPDATE_EVENT(name) \
DEFINE_EVENT(fuse_iext_alt_update_class, name, \
	TP_PROTO(const struct inode *inode, const struct fuse_iomap *map), \
	TP_ARGS(inode, map))
DEFINE_IEXT_ALT_UPDATE_EVENT(fuse_iext_del_mapping_got);
DEFINE_IEXT_ALT_UPDATE_EVENT(fuse_iext_add_mapping_left);
DEFINE_IEXT_ALT_UPDATE_EVENT(fuse_iext_add_mapping_right);

TRACE_EVENT(fuse_iomap_cache_remove,
	TP_PROTO(const struct inode *inode, enum fuse_iomap_fork whichfork,
		 loff_t offset, uint64_t length, unsigned long caller_ip),
	TP_ARGS(inode, whichfork, offset, length, caller_ip),

	TP_STRUCT__entry(
		__field(dev_t,			connection)
		__field(uint64_t,		ino)
		__field(uint64_t,		nodeid)
		__field(loff_t,			isize)
		__field(enum fuse_iomap_fork,	whichfork)
		__field(loff_t,			offset)
		__field(uint64_t,		length)
		__field(unsigned long,		caller_ip)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->whichfork	=	whichfork;
		__entry->offset		=	offset;
		__entry->length		=	length;
		__entry->caller_ip	=	caller_ip;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx whichfork %s offset 0x%llx length 0x%llx caller %pS",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_symbolic(__entry->whichfork, FUSE_IOMAP_FORK_STRINGS),
		  __entry->offset, __entry->length, (void *)__entry->caller_ip)
);

TRACE_EVENT(fuse_iomap_mapping_class,
	TP_PROTO(const struct inode *inode, enum fuse_iomap_fork whichfork,
		 const struct fuse_iomap *map, unsigned long caller_ip),
	TP_ARGS(inode, whichfork, map, caller_ip),

	TP_STRUCT__entry(
		__field(dev_t,			connection)
		__field(uint64_t,		ino)
		__field(uint64_t,		nodeid)
		__field(loff_t,			isize)
		__field(enum fuse_iomap_fork,	whichfork)
		__field(loff_t,			offset)
		__field(loff_t,			length)
		__field(uint16_t,		maptype)
		__field(uint16_t,		mapflags)
		__field(uint32_t,		dev)
		__field(uint64_t,		addr)
		__field(unsigned long,		caller_ip)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->whichfork	=	whichfork;
		__entry->offset		=	map->offset;
		__entry->length		=	map->length;
		__entry->maptype	=	map->type;
		__entry->mapflags	=	map->flags;
		__entry->dev		=	map->dev;
		__entry->addr		=	map->addr;
		__entry->caller_ip	=	caller_ip;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx whichfork %s offset 0x%llx length 0x%llx type %s mapflags (%s) dev %u addr 0x%llx caller %pS",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_symbolic(__entry->whichfork, FUSE_IOMAP_FORK_STRINGS),
		  __entry->offset, __entry->length,
		  __print_symbolic(__entry->maptype, FUSE_IOMAP_TYPE_STRINGS),
		  __print_flags(__entry->mapflags, "|", FUSE_IOMAP_F_STRINGS),
		  __entry->dev, __entry->addr, (void *)__entry->caller_ip)
);
#define DEFINE_FUSE_IOMAP_MAPPING_EVENT(name) \
DEFINE_EVENT(fuse_iomap_mapping_class, name, \
	TP_PROTO(const struct inode *inode, enum fuse_iomap_fork whichfork, \
		 const struct fuse_iomap *map, unsigned long caller_ip), \
	TP_ARGS(inode, whichfork, map, caller_ip))
DEFINE_FUSE_IOMAP_MAPPING_EVENT(fuse_iomap_cache_add);
DEFINE_FUSE_IOMAP_MAPPING_EVENT(fuse_iext_check_mapping);

TRACE_EVENT(fuse_iomap_cache_lookup,
	TP_PROTO(const struct inode *inode, enum fuse_iomap_fork whichfork,
		 loff_t pos, uint64_t count, unsigned long caller_ip),
	TP_ARGS(inode, whichfork, pos, count, caller_ip),

	TP_STRUCT__entry(
		__field(dev_t,			connection)
		__field(uint64_t,		ino)
		__field(uint64_t,		nodeid)
		__field(loff_t,			isize)
		__field(enum fuse_iomap_fork,	whichfork)
		__field(loff_t,			pos)
		__field(uint64_t,		count)
		__field(unsigned long,		caller_ip)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->whichfork	=	whichfork;
		__entry->pos		=	pos;
		__entry->count		=	count;
		__entry->caller_ip	=	caller_ip;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx whichfork %s pos 0x%llx count 0x%llx caller %pS",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_symbolic(__entry->whichfork, FUSE_IOMAP_FORK_STRINGS),
		  __entry->pos, __entry->count,
		  (void *)__entry->caller_ip)
);

TRACE_EVENT(fuse_iomap_cache_lookup_result,
	TP_PROTO(const struct inode *inode, enum fuse_iomap_fork whichfork,
		 loff_t pos, uint64_t count, const struct fuse_iomap *map),
	TP_ARGS(inode, whichfork, pos, count, map),

	TP_STRUCT__entry(
		__field(dev_t,			connection)
		__field(uint64_t,		ino)
		__field(uint64_t,		nodeid)
		__field(loff_t,			isize)
		__field(enum fuse_iomap_fork,	whichfork)
		__field(loff_t,			pos)
		__field(uint64_t,		count)
		__field(loff_t,			offset)
		__field(uint64_t,		length)
		__field(uint16_t,		maptype)
		__field(uint16_t,		mapflags)
		__field(uint32_t,		dev)
		__field(uint64_t,		addr)
		__field(uint64_t,		validity_cookie)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->whichfork	=	whichfork;
		__entry->pos		=	pos;
		__entry->count		=	count;
		__entry->offset		=	map->offset;
		__entry->length		=	map->length;
		__entry->maptype	=	map->type;
		__entry->mapflags	=	map->flags;
		__entry->dev		=	map->dev;
		__entry->addr		=	map->addr;
		__entry->validity_cookie=	map->validity_cookie;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx whichfork %s pos 0x%llx count 0x%llx offset 0x%llx length 0x%llx type %s mapflags (%s) dev %u addr 0x%llx cookie 0x%llx",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
		  __print_symbolic(__entry->whichfork, FUSE_IOMAP_FORK_STRINGS),
		  __entry->pos, __entry->count,
		  __entry->offset, __entry->length,
		  __print_symbolic(__entry->maptype, FUSE_IOMAP_TYPE_STRINGS),
		  __print_flags(__entry->mapflags, "|", FUSE_IOMAP_F_STRINGS),
		  __entry->dev, __entry->addr, __entry->validity_cookie)
);
#endif /* CONFIG_FUSE_IOMAP */

#endif /* _TRACE_FUSE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE fuse_trace
#include <trace/define_trace.h>
