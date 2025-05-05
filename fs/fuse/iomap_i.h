// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef _FS_FUSE_IOMAP_I_H
#define _FS_FUSE_IOMAP_I_H

#if IS_ENABLED(CONFIG_FUSE_IOMAP)
#if IS_ENABLED(CONFIG_FUSE_IOMAP_DEBUG)
# define ASSERT(condition) do {						\
	int __cond = !!(condition);					\
	WARN(!__cond, "Assertion failed: %s, func: %s, line: %d", #condition, __func__, __LINE__); \
} while (0)
# define BAD_DATA(condition) ({						\
	int __cond = !!(condition);					\
	WARN(__cond, "Bad mapping: %s, func: %s, line: %d", #condition, __func__, __LINE__); \
})
#else
# define ASSERT(condition)
# define BAD_DATA(condition) ({						\
	int __cond = !!(condition);					\
	unlikely(__cond);						\
})
#endif /* CONFIG_FUSE_IOMAP_DEBUG */

enum fuse_iomap_iodir {
	READ_MAPPING,
	WRITE_MAPPING,
};

#endif /* CONFIG_FUSE_IOMAP */

#endif /* _FS_FUSE_IOMAP_I_H */
