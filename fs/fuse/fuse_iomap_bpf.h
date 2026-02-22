// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 * Copied from: Joanne Koong <joannelkoong@gmail.com>
 */
#ifndef _FS_FUSE_IOMAP_BPF_H
#define _FS_FUSE_IOMAP_BPF_H

#if IS_ENABLED(CONFIG_FUSE_IOMAP) && IS_ENABLED(CONFIG_BPF_SYSCALL)
enum fuse_iomap_bpf_ret {
	/* fall back to fuse server upcall */
	FIB_FALLBACK = 0,
	/* bpf function handled event completely */
	FIB_HANDLED = 1,
};

struct fuse_iomap_bpf_ops {
	/**
	 * @iomap_begin: override iomap_begin.  See FUSE_IOMAP_BEGIN for
	 * details.
	 */
	enum fuse_iomap_bpf_ret (*iomap_begin)(struct fuse_inode *fi,
			uint64_t pos, uint64_t count, uint32_t opflags,
			struct fuse_iomap_begin_out *outarg);

	/**
	 * @iomap_end: override iomap_end.  See FUSE_IOMAP_END for
	 * details.
	 */
	enum fuse_iomap_bpf_ret (*iomap_end)(struct fuse_inode *fi,
			uint64_t pos, uint64_t count, int64_t written,
			uint32_t opflags);

	/**
	 * @iomap_ioend: override iomap_ioend.  See FUSE_IOMAP_IOEND for
	 * details.
	 */
	enum fuse_iomap_bpf_ret (*iomap_ioend)(struct fuse_inode *fi,
			uint64_t pos, int64_t written, uint32_t ioendflags,
			int error, uint32_t dev, uint64_t new_addr,
			struct fuse_iomap_ioend_out *outarg);

	/**
	 * @fuse_fd: file descriptor of the open fuse device
	 */
	int fuse_fd;

	/**
	 * @zeropad: Explicitly pad to zero.
	 */
	unsigned int zeropad;

	/**
	 * @name: string describing the fuse iomap bpf operations
	 */
	char name[16];

	/* private: don't show fuse connection to the world */
	struct fuse_conn *fc;

	/*
	 * private: number of iomap operations in progress, biased by one for
	 * the fuse connection
	 */
	atomic_t users;
};

int fuse_iomap_init_bpf(void);
void fuse_iomap_unmount_bpf(struct fuse_conn *fc);

int fuse_iomap_begin_bpf(struct inode *inode,
			 const struct fuse_iomap_begin_in *inarg,
			 struct fuse_iomap_begin_out *outarg);
int fuse_iomap_end_bpf(struct inode *inode,
		       const struct fuse_iomap_end_in *inarg);
int fuse_iomap_ioend_bpf(struct inode *inode,
			 const struct fuse_iomap_ioend_in *inarg,
			 struct fuse_iomap_ioend_out *outarg);
#else
# define fuse_iomap_init_bpf()		(0)
# define fuse_iomap_unmount_bpf(...)	((void)0)
# define fuse_iomap_begin_bpf(...)	(-ENOSYS)
# define fuse_iomap_end_bpf(...)	(-ENOSYS)
# define fuse_iomap_ioend_bpf(...)	(-ENOSYS)
#endif /* CONFIG_FUSE_IOMAP && CONFIG_BPF_SYSCALL */

#endif /* _FS_FUSE_IOMAP_BPF_H */
