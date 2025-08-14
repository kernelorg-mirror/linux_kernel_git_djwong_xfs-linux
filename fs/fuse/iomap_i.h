// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef _FS_FUSE_IOMAP_I_H
#define _FS_FUSE_IOMAP_I_H

#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG_DEFAULT)
DECLARE_STATIC_KEY_TRUE(fuse_iomap_debug);
#else
DECLARE_STATIC_KEY_FALSE(fuse_iomap_debug);
#endif

#if IS_ENABLED(CONFIG_FUSE_IOMAP)
#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG)
# define ASSERT(condition) \
while (static_branch_unlikely(&fuse_iomap_debug)) {			\
	int __cond = !!(condition);					\
	if (unlikely(!__cond))						\
		trace_fuse_iomap_assert(__func__, __LINE__, #condition); \
	WARN(!__cond, "Assertion failed: %s, func: %s, line: %d", #condition, __func__, __LINE__); \
	break;								\
}
# define BAD_DATA(condition) ({						\
	int __cond = !!(condition);					\
	if (unlikely(__cond))						\
		trace_fuse_iomap_bad_data(__func__, __LINE__, #condition); \
	if (static_branch_unlikely(&fuse_iomap_debug))			\
		WARN(__cond, "Bad mapping: %s, func: %s, line: %d", #condition, __func__, __LINE__); \
	unlikely(__cond);								\
})
#else
# define ASSERT(condition)
# define BAD_DATA(condition) ({						\
	int __cond = !!(condition);					\
	if (unlikely(__cond))						\
		trace_fuse_iomap_bad_data(__func__, __LINE__, #condition); \
	unlikely(__cond);						\
})
#endif /* CONFIG_FUSE_IOMAP_DEBUG */

enum fuse_iomap_iodir {
	READ_MAPPING,
	WRITE_MAPPING,
};

#define EFSCORRUPTED	EUCLEAN

#endif /* CONFIG_FUSE_IOMAP */

#endif /* _FS_FUSE_IOMAP_I_H */
