// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 * Copied from: Joanne Koong <joannelkoong@gmail.com>
 */
#include <linux/bpf.h>
#include <linux/bpf_verifier.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>

#include "fuse_i.h"
#include "fuse_dev_i.h"
#include "fuse_iomap.h"
#include "fuse_iomap_bpf.h"
#include "fuse_iomap_i.h"
#include "fuse_trace.h"

static const struct btf_type *iomap_begin_out_type, *iomap_ioend_out_type;

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
	const struct btf_type *t = btf_type_by_id(reg->btf, reg->btf_id);

	if (t != iomap_begin_out_type && t != iomap_ioend_out_type) {
		bpf_log(log,
			"Cannot write to memory from a fuse-iomap program\n");
		return -EACCES;
	}

	return 0;
}

static const struct bpf_verifier_ops fuse_iomap_bpf_verifier_ops = {
	.get_func_proto		= bpf_base_func_proto,
	.is_valid_access	= fuse_iomap_bpf_ops_is_valid_access,
	.btf_struct_access	= fuse_iomap_bpf_ops_btf_struct_access,
};

static const struct btf_type *
fuse_iomap_find_struct_type(struct btf *btf, const char *name)
{
	struct btf *some_btf;
	const struct btf_type *ret;
	s32 type_id;

	type_id = bpf_find_btf_id(name, BTF_KIND_STRUCT, &some_btf);
	if (type_id < 0)
		return ERR_PTR(-ENOENT);

	/*
	 * It's only safe to alias a btf_type without a ref to the btf object
	 * if the type is from the current module because the btf object won't
	 * go away until the module unloads.
	 */
	if (some_btf == btf)
		ret = btf_type_by_id(some_btf, type_id);
	else
		ret = ERR_PTR(-ENOENT);
	btf_put(some_btf);

	return ret;
}

static int fuse_iomap_bpf_ops_init(struct btf *btf)
{
	const struct btf_type *t1, *t2;

	t1 = fuse_iomap_find_struct_type(btf, "fuse_iomap_begin_out");
	if (IS_ERR(t1))
		return PTR_ERR(t1);

	t2 = fuse_iomap_find_struct_type(btf, "fuse_iomap_ioend_out");
	if (IS_ERR(t2))
		return PTR_ERR(t2);

	iomap_begin_out_type = t1;
	iomap_ioend_out_type = t2;

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
	case offsetof(struct fuse_iomap_bpf_ops, users):
		ASSERT(atomic_read(&ops->users) == 0);
		atomic_set(&ops->users, 1);
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

	trace_fuse_iomap_attach_bpf(fc, ops);

	/*
	 * The initial ops user count bias is transferred to fc so that we only
	 * initiate wakeup events when someone tries to unregister the BPF.
	 */
	rcu_assign_pointer(fc->iomap_conn.bpf_ops, ops);
	ops->fc = fc;
	spin_unlock(&fuse_iomap_bpf_ops_lock);

	return 0;
}

static inline struct fuse_iomap_bpf_ops *
fuse_iomap_get_bpf_ops(struct inode *inode)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_iomap_bpf_ops *ops;

	rcu_read_lock();
	ops = rcu_dereference(fc->iomap_conn.bpf_ops);
	if (ops && !atomic_inc_not_zero(&ops->users))
		ops = NULL;
	rcu_read_unlock();

	return ops;
}

static inline void
fuse_iomap_put_bpf_ops(struct fuse_iomap_bpf_ops *ops)
{
	if (ops)
		atomic_dec_and_wake_up(&ops->users);
}

DEFINE_CLASS(iomap_bpf_ops, struct fuse_iomap_bpf_ops *,
	     fuse_iomap_put_bpf_ops(_T), fuse_iomap_get_bpf_ops(inode),
	     struct inode *inode);

static void __fuse_iomap_detach_bpf(struct fuse_conn *fc,
				    struct fuse_iomap_bpf_ops *ops)
{
	trace_fuse_iomap_detach_bpf(fc, ops);

	ops->fc = NULL;
	rcu_assign_pointer(fc->iomap_conn.bpf_ops, NULL);
	fuse_iomap_put_bpf_ops(ops);
}

/* Detach any iomap bpf programs from the fuse connection */
void fuse_iomap_unmount_bpf(struct fuse_conn *fc)
{
	spin_lock(&fuse_iomap_bpf_ops_lock);
	if (fc->iomap_conn.bpf_ops) {
		/*
		 * This should only be called from unmount, so there won't be
		 * anybody trying to call the BPF iomap functions.
		 */
		ASSERT(atomic_read(&fc->iomap_conn.bpf_ops->users) == 1);

		__fuse_iomap_detach_bpf(fc, fc->iomap_conn.bpf_ops);
	}
	spin_unlock(&fuse_iomap_bpf_ops_lock);
}

/* Detach the fuse connection from this iomap bpf program */
static void fuse_iomap_bpf_unreg(void *kdata, struct bpf_link *link)
{
	struct fuse_iomap_bpf_ops *ops = kdata;

	spin_lock(&fuse_iomap_bpf_ops_lock);
	if (ops->fc && ops->fc->iomap_conn.bpf_ops == ops)
		__fuse_iomap_detach_bpf(ops->fc, ops);
	spin_unlock(&fuse_iomap_bpf_ops_lock);

	/* wait until nobody's trying to call into the bpf iomap program */
	wait_var_event(&ops->users, atomic_read(&ops->users) == 0);
}

/* Dummy function stubs for control flow integrity hashes */
static enum fuse_iomap_bpf_ret
__iomap_begin(struct fuse_inode *fi, uint64_t pos, uint64_t count,
	      uint32_t opflags, struct fuse_iomap_begin_out *outarg)
{
	return FIB_FALLBACK;
}

static enum fuse_iomap_bpf_ret
__iomap_end(struct fuse_inode *fi, uint64_t pos, uint64_t count,
	    int64_t written, uint32_t opflags)
{
	return FIB_FALLBACK;
}

static enum fuse_iomap_bpf_ret
__iomap_ioend(struct fuse_inode *fi, uint64_t pos, int64_t written,
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

__bpf_kfunc_start_defs();

__bpf_kfunc int
fuse_bpf_iomap_inval_mappings(struct fuse_inode *fi,
			      const struct fuse_range *read__nullable,
			      const struct fuse_range *write__nullable)
{
	struct fuse_iomap_inval_mappings_out outarg = {
		.nodeid = fi->nodeid,
		.attr_ino = fi->orig_ino,
	};
	struct inode *inode = &fi->inode;
	struct fuse_conn *fc = get_fuse_conn(inode);

	if (!fc->iomap)
		return -EOPNOTSUPP;

	if (read__nullable)
		memcpy(&outarg.read, read__nullable, sizeof(outarg.read));
	if (write__nullable)
		memcpy(&outarg.write, write__nullable, sizeof(outarg.write));

	trace_fuse_iomap_inval_mappings(inode, &outarg);

	return fuse_iomap_inval_inode(inode, &outarg);
}

__bpf_kfunc int
fuse_bpf_iomap_upsert_mappings(struct fuse_inode *fi,
			       const struct fuse_iomap_io *read__nullable,
			       const struct fuse_iomap_io *write__nullable)
{
	struct fuse_iomap_upsert_mappings_out outarg = {
		.nodeid = fi->nodeid,
		.attr_ino = fi->orig_ino,
		.read.type = FUSE_IOMAP_TYPE_NOCACHE,
		.write.type = FUSE_IOMAP_TYPE_NOCACHE,
	};
	struct inode *inode = &fi->inode;
	struct fuse_conn *fc = get_fuse_conn(inode);

	if (!fc->iomap)
		return -EOPNOTSUPP;

	if (read__nullable)
		memcpy(&outarg.read, read__nullable, sizeof(outarg.read));
	if (write__nullable)
		memcpy(&outarg.write, write__nullable, sizeof(outarg.write));

	trace_fuse_iomap_upsert_mappings(inode, &outarg);

	return fuse_iomap_upsert_inode(inode, &outarg);
}

__bpf_kfunc_end_defs();

BTF_KFUNCS_START(fuse_iomap_kfunc_ids)
BTF_ID_FLAGS(func, fuse_bpf_iomap_inval_mappings,
	     KF_SLEEPABLE | KF_TRUSTED_ARGS)
BTF_ID_FLAGS(func, fuse_bpf_iomap_upsert_mappings,
	     KF_SLEEPABLE | KF_TRUSTED_ARGS)
BTF_KFUNCS_END(fuse_iomap_kfunc_ids)

static const struct btf_kfunc_id_set fuse_iomap_kfunc_set = {
	.owner = THIS_MODULE,
	.set   = &fuse_iomap_kfunc_ids,
};

/* Register the iomap bpf ops so that fuse servers can attach to it */
int __init fuse_iomap_init_bpf(void)
{
	int ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
			&fuse_iomap_kfunc_set);
	if (ret)
		return ret;

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
	CLASS(iomap_bpf_ops, bpf_ops)(inode);
	enum fuse_iomap_bpf_ret ret;

	if (!bpf_ops || !bpf_ops->iomap_begin)
		return -ENOSYS;

	trace_fuse_iomap_begin_bpf(inode);

	ret = bpf_ops->iomap_begin(fi, inarg->pos, inarg->count,
				   inarg->opflags, outarg);
	return bpf_to_errno(ret);
}

/* Try to call the bpf version of ->iomap_end */
int fuse_iomap_end_bpf(struct inode *inode,
		       const struct fuse_iomap_end_in *inarg)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	CLASS(iomap_bpf_ops, bpf_ops)(inode);
	enum fuse_iomap_bpf_ret ret;

	if (!bpf_ops || !bpf_ops->iomap_end)
		return -ENOSYS;

	trace_fuse_iomap_end_bpf(inode);

	ret = bpf_ops->iomap_end(fi, inarg->pos, inarg->count,
				 inarg->written, inarg->opflags);
	return bpf_to_errno(ret);
}

/* Try to call the bpf version of ->iomap_ioend */
int fuse_iomap_ioend_bpf(struct inode *inode,
			 const struct fuse_iomap_ioend_in *inarg,
			 struct fuse_iomap_ioend_out *outarg)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	CLASS(iomap_bpf_ops, bpf_ops)(inode);
	enum fuse_iomap_bpf_ret ret;

	if (!bpf_ops || !bpf_ops->iomap_ioend)
		return -ENOSYS;

	trace_fuse_iomap_ioend_bpf(inode);

	ret = bpf_ops->iomap_ioend(fi, inarg->pos, inarg->written,
				   inarg->flags, inarg->error, inarg->dev,
				   inarg->new_addr, outarg);
	return bpf_to_errno(ret);
}
