// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 * Copied from: Joanne Koong <joannelkoong@gmail.com>
 */
#include <linux/bpf.h>

#include "fuse_i.h"
#include "fuse_dev_i.h"
#include "fuse_iomap_bpf.h"

static struct btf *fuse_iomap_bpf_ops_btf;

/* spinlock for atomically updating fuse_conn <-> bpf_ops pointers */
static DEFINE_SPINLOCK(fuse_iomap_bpf_ops_lock);

/*
 * The only structures that we provide to the BPF program are outparams, so
 * they can write anything they want to it.
 */
static bool fuse_iomap_bpf_ops_is_valid_access(int off, int size,
					       enum bpf_access_type type,
					       const struct bpf_prog *prog,
					       struct bpf_insn_access_aux *info)
{
	return bpf_tracing_btf_ctx_access(off, size, type, prog, info);
}

static int fuse_iomap_bpf_ops_check_member(const struct btf_type *t,
					   const struct btf_member *member,
					   const struct bpf_prog *prog)
{
	return 0;
}

static int fuse_iomap_bpf_ops_btf_struct_access(struct bpf_verifier_log *log,
						const struct bpf_reg_state *reg,
						int off, int size)
{
	return 0;
}

static const struct bpf_verifier_ops fuse_iomap_bpf_verifier_ops = {
	.get_func_proto		= bpf_base_func_proto,
	.is_valid_access	= fuse_iomap_bpf_ops_is_valid_access,
	.btf_struct_access	= fuse_iomap_bpf_ops_btf_struct_access,
};

static int fuse_iomap_bpf_ops_init(struct btf *btf)
{
	fuse_iomap_bpf_ops_btf = btf;
	return 0;
}

/* Copy data from userspace bpf ops to the kernel */
static int fuse_iomap_bpf_ops_init_member(const struct btf_type *t,
					  const struct btf_member *member,
					  void *kdata, const void *udata)
{
	const struct fuse_iomap_bpf_ops *u_ops = udata;
	struct fuse_iomap_bpf_ops *ops = kdata;
	u32 moff;

	/*
	 * This function must copy all non-function-pointers by itself and
	 * return 1 to indicate that the data has been handled by the
	 * struct_ops type, or the verifier will reject the map if the value of
	 * those fields is not zero.
	 */
	moff = __btf_member_bit_offset(t, member) / 8;
	switch (moff) {
	case offsetof(struct fuse_iomap_bpf_ops, fuse_fd):
		ops->fuse_fd = u_ops->fuse_fd;
		return 1;
	case offsetof(struct fuse_iomap_bpf_ops, name):
		if (bpf_obj_name_cpy(ops->name, u_ops->name,
				     sizeof(ops->name)) <= 0)
			return -EINVAL;
		return 1;  /* Handled */
	}

	/* Not handled, use default */
	return 0;
}

/* Register an iomap bpf program with a fuse connection */
static int fuse_iomap_bpf_reg(void *kdata, struct bpf_link *link)
{
	struct fuse_iomap_bpf_ops *ops = kdata;
	struct file *fusedev_file;
	struct fuse_dev *fud;
	struct fuse_conn *fc;

	CLASS(fd, fusedev_fd)(ops->fuse_fd);
	if (fd_empty(fusedev_fd))
		return -EBADF;

	fusedev_file = fd_file(fusedev_fd);
	if (fusedev_file->f_op != &fuse_dev_operations)
		return -EBADF;

	fud = fuse_get_dev(fusedev_file);
	fc = fud->fc;

	if (!fc->iomap)
		return -EOPNOTSUPP;

	spin_lock(&fuse_iomap_bpf_ops_lock);
	if (fc->iomap_conn.bpf_ops) {
		spin_unlock(&fuse_iomap_bpf_ops_lock);
		return -EBUSY;
	}

	fc->iomap_conn.bpf_ops = ops;
	ops->fc = fc;
	spin_unlock(&fuse_iomap_bpf_ops_lock);

	return 0;
}

/* Detach any iomap bpf programs from the fuse connection */
void fuse_iomap_detach_bpf(struct fuse_conn *fc)
{
	spin_lock(&fuse_iomap_bpf_ops_lock);
	if (fc->iomap_conn.bpf_ops) {
		fc->iomap_conn.bpf_ops->fc = NULL;
		fc->iomap_conn.bpf_ops = NULL;
	}
	spin_unlock(&fuse_iomap_bpf_ops_lock);
}

/* Detach the fuse connection from this iomap bpf program */
static void fuse_iomap_bpf_unreg(void *kdata, struct bpf_link *link)
{
	struct fuse_iomap_bpf_ops *ops = kdata;

	spin_lock(&fuse_iomap_bpf_ops_lock);
	if (ops->fc && ops->fc->iomap_conn.bpf_ops == ops) {
		ops->fc->iomap_conn.bpf_ops = NULL;
		ops->fc = NULL;
	}
	spin_unlock(&fuse_iomap_bpf_ops_lock);
}

/* Dummy function stubs for control flow integrity hashes */
static enum fuse_iomap_bpf_ret
__iomap_begin(uint64_t nodeid, uint64_t pos, uint64_t count, uint32_t opflags,
	      struct fuse_iomap_begin_out *outarg)
{
	return FIB_FALLBACK;
}

static enum fuse_iomap_bpf_ret
__iomap_end(uint64_t nodeid, uint64_t pos, uint64_t count, int64_t written,
	    uint32_t opflags)
{
	return FIB_FALLBACK;
}

static enum fuse_iomap_bpf_ret
__iomap_ioend(uint64_t nodeid, uint64_t pos, int64_t written,
	      uint32_t ioendflags, int error, uint32_t dev, uint64_t new_addr,
	      struct fuse_iomap_ioend_out *outarg)
{
	return FIB_FALLBACK;
}

static struct fuse_iomap_bpf_ops __fuse_iomap_bpf_ops = {
	.iomap_begin	= __iomap_begin,
	.iomap_end	= __iomap_end,
	.iomap_ioend	= __iomap_ioend,
};

static struct bpf_struct_ops fuse_iomap_bpf_struct_ops = {
	.verifier_ops	= &fuse_iomap_bpf_verifier_ops,
	.init		= fuse_iomap_bpf_ops_init,
	.check_member	= fuse_iomap_bpf_ops_check_member,
	.init_member	= fuse_iomap_bpf_ops_init_member,
	.reg		= fuse_iomap_bpf_reg,
	.unreg		= fuse_iomap_bpf_unreg,
	.name		= "fuse_iomap_bpf_ops",
	.cfi_stubs	= &__fuse_iomap_bpf_ops,
	.owner		= THIS_MODULE,
};

/* Register the iomap bpf ops so that fuse servers can attach to it */
int __init fuse_iomap_init_bpf(void)
{
	return register_bpf_struct_ops(&fuse_iomap_bpf_struct_ops,
				       fuse_iomap_bpf_ops);
}

/* Register key structures with BTF so that BPF programs can use structs */
BTF_ID_LIST_GLOBAL_SINGLE(btf_fuse_iomap_bpf_ops_id,
		struct, fuse_iomap_bpf_ops)
BTF_ID_LIST_GLOBAL_SINGLE(btf_fuse_iomap_begin_out_id,
		struct, fuse_iomap_begin_out)
BTF_ID_LIST_GLOBAL_SINGLE(btf_fuse_iomap_ioend_out_id,
		struct, fuse_iomap_ioend_out)

static inline struct fuse_iomap_bpf_ops *
fuse_iomap_get_bpf_ops(struct inode *inode)
{
	struct fuse_conn *fc = get_fuse_conn(inode);

	return fc->iomap_conn.bpf_ops;
}

static inline int bpf_to_errno(enum fuse_iomap_bpf_ret ret)
{
	switch (ret) {
	case FIB_HANDLED:
		return 0;
	case FIB_FALLBACK:
	default:
		return -ENOSYS;
	}
}

/* Try to call the bpf version of ->iomap_begin */
int fuse_iomap_begin_bpf(struct inode *inode,
			 const struct fuse_iomap_begin_in *inarg,
			 struct fuse_iomap_begin_out *outarg)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_bpf_ops *bpf_ops = fuse_iomap_get_bpf_ops(inode);
	enum fuse_iomap_bpf_ret ret;

	if (!bpf_ops || !bpf_ops->iomap_begin)
		return -ENOSYS;

	ret = bpf_ops->iomap_begin(fi->nodeid, inarg->pos, inarg->count,
				   inarg->opflags, outarg);
	return bpf_to_errno(ret);
}

/* Try to call the bpf version of ->iomap_end */
int fuse_iomap_end_bpf(struct inode *inode,
		       const struct fuse_iomap_end_in *inarg)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_bpf_ops *bpf_ops = fuse_iomap_get_bpf_ops(inode);
	enum fuse_iomap_bpf_ret ret;

	if (!bpf_ops || !bpf_ops->iomap_end)
		return -ENOSYS;

	ret = bpf_ops->iomap_end(fi->nodeid, inarg->pos, inarg->count,
				 inarg->written, inarg->opflags);
	return bpf_to_errno(ret);
}

/* Try to call the bpf version of ->iomap_ioend */
int fuse_iomap_ioend_bpf(struct inode *inode,
			 const struct fuse_iomap_ioend_in *inarg,
			 struct fuse_iomap_ioend_out *outarg)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_iomap_bpf_ops *bpf_ops = fuse_iomap_get_bpf_ops(inode);
	enum fuse_iomap_bpf_ret ret;

	if (!bpf_ops || !bpf_ops->iomap_ioend)
		return -ENOSYS;

	ret = bpf_ops->iomap_ioend(fi->nodeid, inarg->pos, inarg->written,
				   inarg->flags, inarg->error, inarg->dev,
				   inarg->new_addr, outarg);
	return bpf_to_errno(ret);
}
