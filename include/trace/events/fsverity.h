/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM fsverity

#if !defined(_TRACE_FSVERITY_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_FSVERITY_H

#include <linux/tracepoint.h>

struct fsverity_descriptor;
struct merkle_tree_params;
struct fsverity_info;

TRACE_EVENT(fsverity_enable,
	TP_PROTO(const struct inode *inode,
		 const struct merkle_tree_params *params),
	TP_ARGS(inode, params),
	TP_STRUCT__entry(
		__field(ino_t, ino)
		__field(u64, data_size)
		__field(unsigned int, block_size)
		__field(unsigned int, num_levels)
		__field(u64, tree_size)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
		__entry->data_size = i_size_read(inode);
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

TRACE_EVENT(fsverity_disable,
	TP_PROTO(const struct inode *inode),
	TP_ARGS(inode),
	TP_STRUCT__entry(
		__field(ino_t, ino)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
	),
	TP_printk("ino %lu",
		(unsigned long) __entry->ino)
);

TRACE_EVENT(fsverity_tree_done,
	TP_PROTO(const struct inode *inode, const struct fsverity_info *vi,
		 const struct merkle_tree_params *params),
	TP_ARGS(inode, vi, params),
	TP_STRUCT__entry(
		__field(ino_t, ino)
		__field(unsigned int, levels)
		__field(unsigned int, block_size)
		__field(u64, tree_size)
		__dynamic_array(u8, root_hash, params->digest_size)
		__dynamic_array(u8, file_digest, params->digest_size)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
		__entry->levels = params->num_levels;
		__entry->block_size = params->block_size;
		__entry->tree_size = params->tree_size;
		memcpy(__get_dynamic_array(root_hash), vi->root_hash, __get_dynamic_array_len(root_hash));
		memcpy(__get_dynamic_array(file_digest), vi->file_digest, __get_dynamic_array_len(file_digest));
	),
	TP_printk("ino %lu levels %d block_size %d tree_size %lld root_hash %s digest %s",
		(unsigned long) __entry->ino,
		__entry->levels,
		__entry->block_size,
		__entry->tree_size,
		__print_hex_str(__get_dynamic_array(root_hash), __get_dynamic_array_len(root_hash)),
		__print_hex_str(__get_dynamic_array(file_digest), __get_dynamic_array_len(file_digest)))
);

TRACE_EVENT(fsverity_verify_data_block,
	TP_PROTO(const struct inode *inode,
		 const struct merkle_tree_params *params,
		 u64 data_pos),
	TP_ARGS(inode, params, data_pos),
	TP_STRUCT__entry(
		__field(ino_t, ino)
		__field(u64, data_pos)
		__field(unsigned int, block_size)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
		__entry->data_pos = data_pos;
		__entry->block_size = params->block_size;
	),
	TP_printk("ino %lu pos %lld merkle_blocksize %u",
		(unsigned long) __entry->ino,
		__entry->data_pos,
		__entry->block_size)
);

TRACE_EVENT(fsverity_merkle_hit,
	TP_PROTO(const struct inode *inode, u64 data_pos, u64 merkle_pos,
		 unsigned int level, unsigned int hidx),
	TP_ARGS(inode, data_pos, merkle_pos, level, hidx),
	TP_STRUCT__entry(
		__field(ino_t, ino)
		__field(u64, data_pos)
		__field(u64, merkle_pos)
		__field(unsigned int, level)
		__field(unsigned int, hidx)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
		__entry->data_pos = data_pos;
		__entry->merkle_pos = merkle_pos;
		__entry->level = level;
		__entry->hidx = hidx;
	),
	TP_printk("ino %lu data_pos %llu merkle_pos %llu level %u hidx %u",
		(unsigned long) __entry->ino,
		__entry->data_pos,
		__entry->merkle_pos,
		__entry->level,
		__entry->hidx)
);

TRACE_EVENT(fsverity_verify_merkle_block,
	TP_PROTO(const struct inode *inode, u64 merkle_pos, unsigned int level,
		unsigned int hidx),
	TP_ARGS(inode, merkle_pos, level, hidx),
	TP_STRUCT__entry(
		__field(ino_t, ino)
		__field(u64, merkle_pos)
		__field(unsigned int, level)
		__field(unsigned int, hidx)
	),
	TP_fast_assign(
		__entry->ino = inode->i_ino;
		__entry->merkle_pos = merkle_pos;
		__entry->level = level;
		__entry->hidx = hidx;
	),
	TP_printk("ino %lu merkle_pos %llu level %u hidx %u",
		(unsigned long) __entry->ino,
		__entry->merkle_pos,
		__entry->level,
		__entry->hidx)
);

#endif /* _TRACE_FSVERITY_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
