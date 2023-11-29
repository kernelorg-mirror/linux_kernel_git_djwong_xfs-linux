/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM fsverity

#if !defined(_TRACE_FSVERITY_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_FSVERITY_H

#include <linux/tracepoint.h>

struct fsverity_descriptor;
struct merkle_tree_params;
struct fsverity_info;

#define FSVERITY_TRACE_DIR_ASCEND	(1ul << 0)
#define FSVERITY_TRACE_DIR_DESCEND	(1ul << 1)
#define FSVERITY_HASH_SHOWN_LEN		20

TRACE_EVENT(fsverity_enable,
	TP_PROTO(struct inode *inode, struct fsverity_descriptor *desc,
		struct merkle_tree_params *params),
	TP_ARGS(inode, desc, params),
	TP_STRUCT__entry(
		__field(ino_t, ino)
		__field(u64, data_size)
		__field(unsigned int, block_size)
		__field(unsigned int, num_levels)
		__field(u64, tree_size)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
		__entry->data_size = desc->data_size;
		__entry->block_size = params->block_size;
		__entry->num_levels = params->num_levels;
		__entry->tree_size = params->tree_size;
	),
	TP_printk("ino %lu data size %llu tree size %llu block size %u levels %u",
		(unsigned long) __entry->ino,
		__entry->data_size,
		__entry->tree_size,
		__entry->block_size,
		__entry->num_levels)
);

TRACE_EVENT(fsverity_tree_done,
	TP_PROTO(struct inode *inode, struct fsverity_descriptor *desc,
		struct merkle_tree_params *params),
	TP_ARGS(inode, desc, params),
	TP_STRUCT__entry(
		__field(ino_t, ino)
		__field(unsigned int, levels)
		__field(unsigned int, tree_blocks)
		__field(u64, tree_size)
		__array(u8, tree_hash, 64)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
		__entry->levels = params->num_levels;
		__entry->tree_blocks =
			params->tree_size >> params->log_blocksize;
		__entry->tree_size = params->tree_size;
		memcpy(__entry->tree_hash, desc->root_hash, 64);
	),
	TP_printk("ino %lu levels %d tree_blocks %d tree_size %lld root_hash %s",
		(unsigned long) __entry->ino,
		__entry->levels,
		__entry->tree_blocks,
		__entry->tree_size,
		__print_hex(__entry->tree_hash, 64))
);

TRACE_EVENT(fsverity_verify_block,
	TP_PROTO(struct inode *inode, u64 offset),
	TP_ARGS(inode, offset),
	TP_STRUCT__entry(
		__field(ino_t, ino)
		__field(u64, offset)
		__field(unsigned int, block_size)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
		__entry->offset = offset;
		__entry->block_size =
			inode->i_verity_info->tree_params.block_size;
	),
	TP_printk("ino %lu data offset %lld data block size %u",
		(unsigned long) __entry->ino,
		__entry->offset,
		__entry->block_size)
);

TRACE_EVENT(fsverity_merkle_tree_block_verified,
	TP_PROTO(struct inode *inode,
		 struct fsverity_blockbuf *block,
		 u8 direction),
	TP_ARGS(inode, block, direction),
	TP_STRUCT__entry(
		__field(ino_t, ino)
		__field(u64, offset)
		__field(u8, direction)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
		__entry->offset = block->offset;
		__entry->direction = direction;
	),
	TP_printk("ino %lu block offset %llu %s",
		(unsigned long) __entry->ino,
		__entry->offset,
		__entry->direction == 0 ? "ascend" : "descend")
);

TRACE_EVENT(fsverity_invalidate_block,
	TP_PROTO(struct inode *inode, struct fsverity_blockbuf *block),
	TP_ARGS(inode, block),
	TP_STRUCT__entry(
		__field(ino_t, ino)
		__field(u64, offset)
		__field(unsigned int, block_size)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
		__entry->offset = block->offset;
		__entry->block_size = block->size;
	),
	TP_printk("ino %lu block position %llu block size %u",
		(unsigned long) __entry->ino,
		__entry->offset,
		__entry->block_size)
);

TRACE_EVENT(fsverity_read_merkle_tree_block,
	TP_PROTO(struct inode *inode, u64 offset, unsigned int log_blocksize),
	TP_ARGS(inode, offset, log_blocksize),
	TP_STRUCT__entry(
		__field(ino_t, ino)
		__field(u64, offset)
		__field(u64, index)
		__field(unsigned int, block_size)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
		__entry->offset = offset;
		__entry->index = offset >> log_blocksize;
		__entry->block_size = 1 << log_blocksize;
	),
	TP_printk("ino %lu tree offset %llu block index %llu block hize %u",
		(unsigned long) __entry->ino,
		__entry->offset,
		__entry->index,
		__entry->block_size)
);

TRACE_EVENT(fsverity_verify_signature,
	TP_PROTO(const struct inode *inode, const u8 *signature, size_t sig_size),
	TP_ARGS(inode, signature, sig_size),
	TP_STRUCT__entry(
		__field(ino_t, ino)
		__dynamic_array(u8, signature, sig_size)
		__field(size_t, sig_size)
		__field(size_t, sig_size_show)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
		memcpy(__get_dynamic_array(signature), signature, sig_size);
		__entry->sig_size = sig_size;
		__entry->sig_size_show = (sig_size > FSVERITY_HASH_SHOWN_LEN ?
			FSVERITY_HASH_SHOWN_LEN : sig_size);
	),
	TP_printk("ino %lu sig_size %lu %s%s%s",
		(unsigned long) __entry->ino,
		__entry->sig_size,
		(__entry->sig_size ? "sig " : ""),
		__print_hex(__get_dynamic_array(signature),
			__entry->sig_size_show),
		(__entry->sig_size ? "..." : ""))
);

#endif /* _TRACE_FSVERITY_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
