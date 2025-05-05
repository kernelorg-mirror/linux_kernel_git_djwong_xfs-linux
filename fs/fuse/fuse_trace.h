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

TRACE_DEFINE_ENUM(FUSE_I_ADVISE_RDPLUS);
TRACE_DEFINE_ENUM(FUSE_I_INIT_RDPLUS);
TRACE_DEFINE_ENUM(FUSE_I_SIZE_UNSTABLE);
TRACE_DEFINE_ENUM(FUSE_I_BAD);
TRACE_DEFINE_ENUM(FUSE_I_BTIME);
TRACE_DEFINE_ENUM(FUSE_I_CACHE_IO_MODE);
TRACE_DEFINE_ENUM(FUSE_I_IOMAP_DIRECTIO);

#define FUSE_IFLAG_STRINGS \
	{ 1 << FUSE_I_ADVISE_RDPLUS,		"advise_rdplus" }, \
	{ 1 << FUSE_I_INIT_RDPLUS,		"init_rdplus" }, \
	{ 1 << FUSE_I_SIZE_UNSTABLE,		"size_unstable" }, \
	{ 1 << FUSE_I_BAD,			"bad" }, \
	{ 1 << FUSE_I_BTIME,			"btime" }, \
	{ 1 << FUSE_I_CACHE_IO_MODE,		"cacheio" }, \
	{ 1 << FUSE_I_IOMAP_DIRECTIO,		"iomap_dio" }

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

DECLARE_EVENT_CLASS(fuse_inode_state_class,
	TP_PROTO(const struct inode *inode),
	TP_ARGS(inode),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	ino)
		__field(uint64_t,	nodeid)
		__field(loff_t,		isize)
		__field(unsigned long,	state)
	),

	TP_fast_assign(
		const struct fuse_inode *fi = get_fuse_inode_c(inode);
		const struct fuse_mount *fm = get_fuse_mount_c(inode);

		__entry->connection	=	fm->fc->dev;
		__entry->ino		=	fi->orig_ino;
		__entry->nodeid		=	fi->nodeid;
		__entry->isize		=	i_size_read(inode);
		__entry->state		=	fi->state;
	),

	TP_printk("connection %u ino %llu nodeid %llu isize 0x%llx state (%s)",
		  __entry->connection, __entry->ino, __entry->nodeid,
		  __entry->isize,
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
