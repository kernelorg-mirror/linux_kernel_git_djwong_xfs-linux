/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Red Hat, Inc.
 */
#include "xfs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_log_format.h"
#include "xfs_attr.h"
#include "xfs_verity.h"
#include "xfs_bmap_util.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_attr_leaf.h"
#include "xfs_trace.h"
#include "xfs_quota.h"
#include "xfs_ag.h"
#include "xfs_fsverity.h"
#include <linux/fsverity.h>

/*
 * Merkle Tree Block Cache
 * =======================
 *
 * fsverity requires that the filesystem implement caching of ondisk merkle
 * tree blocks.  XFS stores merkle tree blocks in the extended attribute data,
 * which makes it important to keep copies in memory for as long as possible.
 * This is performed by allocating the data blob structure defined below,
 * passing the data portion of the blob to xfs_attr_get, and later caching the
 * data blob via a per-ag hashtable.
 *
 * The cache structure indexes merkle tree blocks by the pos given to us by
 * fsverity, which drastically reduces lookups.  First, it eliminating the need
 * to walk the xattr structure to find the remote block containing the merkle
 * tree block.  Second, access to each block in the xattr structure requires a
 * lookup in the incore extent btree.
 */
struct xfs_merkle_blob {
	struct rhash_head	rhash;
	struct rcu_head		rcu;

	struct xfs_merkle_bkey	key;

	/* refcount of this item; the cache holds its own ref */
	refcount_t		refcount;

	unsigned long		flags;

	/* Pointer to the merkle tree block, which is power-of-2 sized */
	void			*data;
};

#define XFS_MERKLE_BLOB_VERIFIED_BIT	(0) /* fsverity validated this */

static const struct rhashtable_params xfs_fsverity_merkle_hash_params = {
	.key_len		= sizeof(struct xfs_merkle_bkey),
	.key_offset		= offsetof(struct xfs_merkle_blob, key),
	.head_offset		= offsetof(struct xfs_merkle_blob, rhash),
	.automatic_shrinking	= true,
};

/*
 * Allocate a merkle tree blob object to prepare for reading a merkle tree
 * object from disk.
 */
static inline struct xfs_merkle_blob *
xfs_merkle_blob_alloc(
	struct xfs_inode	*ip,
	u64			pos,
	unsigned int		blocksize)
{
	struct xfs_merkle_blob	*mk;

	mk = kmalloc(sizeof(struct xfs_merkle_blob), GFP_KERNEL);
	if (!mk)
		return NULL;

	mk->data = kvzalloc(blocksize, GFP_KERNEL);
	if (!mk->data) {
		kfree(mk);
		return NULL;
	}

	/* Caller owns this refcount. */
	refcount_set(&mk->refcount, 1);
	mk->flags = 0;
	mk->key.ino = ip->i_ino;
	mk->key.pos = pos;
	return mk;
}

/* Actually free this blob. */
static void
xfs_merkle_blob_free(
	struct callback_head	*cb)
{
	struct xfs_merkle_blob	*mk =
		container_of(cb, struct xfs_merkle_blob, rcu);

	kvfree(mk->data);
	kfree(mk);
}

/* Free a merkle tree blob. */
static inline void
xfs_merkle_blob_rele(
	struct xfs_merkle_blob	*mk)
{
	if (refcount_dec_and_test(&mk->refcount))
		call_rcu(&mk->rcu, xfs_merkle_blob_free);
}

/*
 * Drop this merkle tree blob from the cache.  Caller must have a reference to
 * the blob, which will be dropped at the end.
 */
static inline void
xfs_merkle_blob_drop(
	struct xfs_perag	*pag,
	struct xfs_merkle_blob	*mk)
{
	/*
	 * Remove the blob from the hash table and drop the cache's
	 * ref to the blob handle.
	 */
	spin_lock(&pag->pagi_merkle_lock);
	rhashtable_remove_fast(&pag->pagi_merkle_blobs, &mk->rhash,
			xfs_fsverity_merkle_hash_params);
	xfs_merkle_blob_rele(mk);
	spin_unlock(&pag->pagi_merkle_lock);

	/* Drop the reference we obtained above. */
	xfs_merkle_blob_rele(mk);
}

/* Drop all the merkle tree blocks from this part of the cache. */
STATIC void
xfs_fsverity_drop_cache(
	struct xfs_inode	*ip,
	u64			tree_size,
	unsigned int		block_size)
{
	struct xfs_merkle_bkey	key = {
		.ino		= ip->i_ino,
		.pos		= 0,
	};
	struct xfs_perag	*pag;
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_merkle_blob	*mk;
	s64			freed = 0;

	pag = xfs_perag_get(mp, XFS_INO_TO_AGNO(mp, ip->i_ino));
	if (!pag)
		return;

	for (key.pos = 0; key.pos < tree_size; key.pos += block_size) {
		/*
		 * Try to grab the blob from the hash table and get our own
		 * reference to the object.  If there's a blob handle but it
		 * has zero refcount then we're racing with reclaim and can
		 * move on.
		 */
		rcu_read_lock();
		mk = rhashtable_lookup(&pag->pagi_merkle_blobs, &key,
				xfs_fsverity_merkle_hash_params);
		if (mk && !refcount_inc_not_zero(&mk->refcount))
			mk = NULL;
		rcu_read_unlock();

		if (!mk)
			continue;

		trace_xfs_fsverity_cache_drop(mp, &mk->key, _RET_IP_);

		xfs_merkle_blob_drop(pag, mk);
		freed++;
	}

	xfs_perag_put(pag);
}

/*
 * Drop all the merkle tree blocks out of the cache.  Caller must ensure that
 * there are no active references to cache items.
 */
void
xfs_fsverity_destroy_inode(
	struct xfs_inode	*ip)
{
	u64			tree_size;
	unsigned int		block_size;
	int			error;

	error = fsverity_merkle_tree_geometry(VFS_I(ip), &block_size,
			&tree_size);
	if (error)
		return;

	xfs_fsverity_drop_cache(ip, tree_size, block_size);
}

/* Return a cached merkle tree block, or NULL. */
static struct xfs_merkle_blob *
xfs_fsverity_cache_load(
	struct xfs_inode	*ip,
	u64			pos)
{
	struct xfs_merkle_bkey	key = {
		.ino		= ip->i_ino,
		.pos		= pos,
	};
	struct xfs_perag	*pag;
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_merkle_blob	*mk;

	pag = xfs_perag_get(mp, XFS_INO_TO_AGNO(mp, ip->i_ino));
	if (!pag)
		return NULL;

	rcu_read_lock();
	mk = rhashtable_lookup(&pag->pagi_merkle_blobs, &key,
			xfs_fsverity_merkle_hash_params);
	if (mk && !refcount_inc_not_zero(&mk->refcount))
		mk = NULL;
	rcu_read_unlock();
	xfs_perag_put(pag);

	if (!mk) {
		trace_xfs_fsverity_cache_miss(mp, &key, _RET_IP_);
		return NULL;
	}

	trace_xfs_fsverity_cache_hit(mp, &mk->key, _RET_IP_);
	return mk;
}

/*
 * Try to store a merkle tree block in the cache with the given key.
 *
 * If the merkle tree block is not already in the cache, the given block @mk
 * will be added to the cache and returned.  The caller retains its active
 * reference to @mk.
 *
 * If there was already a merkle block in the cache, it will be returned to
 * the caller with an active reference.  @mk will be untouched.
 */
static struct xfs_merkle_blob *
xfs_fsverity_cache_store(
	struct xfs_inode	*ip,
	struct xfs_merkle_blob	*mk)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_merkle_blob	*old;
	struct xfs_perag	*pag;

	ASSERT(ip->i_ino == mk->key.ino);

	pag = xfs_perag_get(mp, XFS_INO_TO_AGNO(mp, ip->i_ino));
	if (!pag) {
		ASSERT(pag);
		return ERR_PTR(-EFSCORRUPTED);
	}

	spin_lock(&pag->pagi_merkle_lock);
	old = rhashtable_lookup_get_insert_fast(&pag->pagi_merkle_blobs,
			&mk->rhash, xfs_fsverity_merkle_hash_params);
	if (IS_ERR(old)) {
		spin_unlock(&pag->pagi_merkle_lock);
		xfs_perag_put(pag);
		return old;
	}
	if (!old) {
		/*
		 * There was no previous value.  @mk is now live in the cache.
		 * Bump the active refcount to transfer ownership to the cache
		 * and return @mk to the caller.
		 */
		refcount_inc(&mk->refcount);
		spin_unlock(&pag->pagi_merkle_lock);
		xfs_perag_put(pag);

		trace_xfs_fsverity_cache_store(mp, &mk->key, _RET_IP_);
		return mk;
	}

	/*
	 * We obtained an active reference to a previous value in the cache.
	 * Return it to the caller.
	 */
	refcount_inc(&old->refcount);
	spin_unlock(&pag->pagi_merkle_lock);
	xfs_perag_put(pag);

	trace_xfs_fsverity_cache_reuse(mp, &old->key, _RET_IP_);
	return old;
}

/* Set up fsverity for this mount. */
int
xfs_fsverity_mount(
	struct xfs_mount	*mp)
{
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno;
	int			error;

	if (!xfs_has_verity(mp))
		return 0;

	for_each_perag(mp, agno, pag) {
		spin_lock_init(&pag->pagi_merkle_lock);
		error = rhashtable_init(&pag->pagi_merkle_blobs,
				&xfs_fsverity_merkle_hash_params);
		if (error) {
			xfs_perag_put(pag);
			goto out_perag;
		}
		set_bit(XFS_AGSTATE_MERKLE, &pag->pag_opstate);
	}

	return 0;
out_perag:
	for_each_perag(mp, agno, pag) {
		if (test_and_clear_bit(XFS_AGSTATE_MERKLE, &pag->pag_opstate))
			rhashtable_destroy(&pag->pagi_merkle_blobs);
	}

	return error;
}

/* Set up new merkle tree caches for new AGs. */
int
xfs_fsverity_growfs(
	struct xfs_mount	*mp,
	xfs_agnumber_t		old_agcount,
	xfs_agnumber_t		new_agcount)
{
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno;
	int			error;

	if (!xfs_has_verity(mp))
		return 0;

	agno = old_agcount;
	for_each_perag_range(mp, agno, new_agcount - 1, pag) {
		spin_lock_init(&pag->pagi_merkle_lock);
		error = rhashtable_init(&pag->pagi_merkle_blobs,
				&xfs_fsverity_merkle_hash_params);
		if (error) {
			xfs_perag_put(pag);
			goto out_perag;
		}
		set_bit(XFS_AGSTATE_MERKLE, &pag->pag_opstate);
	}

	return 0;
out_perag:
	agno = old_agcount;
	for_each_perag_range(mp, agno, new_agcount - 1, pag) {
		if (test_and_clear_bit(XFS_AGSTATE_MERKLE, &pag->pag_opstate))
			rhashtable_destroy(&pag->pagi_merkle_blobs);
	}

	return error;
}

struct xfs_fsverity_umount {
	struct xfs_mount	*mp;
	s64			freed;
};

/* Destroy this blob that's still left over in the cache. */
static void
xfs_merkle_blob_destroy(
	void			*ptr,
	void			*arg)
{
	struct xfs_fsverity_umount *fu = arg;
	struct xfs_merkle_blob	*mk = ptr;

	trace_xfs_fsverity_cache_unmount(fu->mp, &mk->key, _RET_IP_);

	xfs_merkle_blob_rele(ptr);
	fu->freed++;
}

/* Tear down fsverity from this mount. */
void
xfs_fsverity_unmount(
	struct xfs_mount	*mp)
{
	struct xfs_fsverity_umount fu = {
		.mp		= mp,
		.freed		= 0,
	};
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno;

	if (!xfs_has_verity(mp))
		return;

	for_each_perag(mp, agno, pag) {
		if (test_and_clear_bit(XFS_AGSTATE_MERKLE, &pag->pag_opstate))
			rhashtable_free_and_destroy(&pag->pagi_merkle_blobs,
					xfs_merkle_blob_destroy, &fu);
	}
}

/*
 * Initialize an args structure to load or store the fsverity descriptor.
 * Caller must ensure @args is zeroed except for value and valuelen.
 */
static inline void
xfs_fsverity_init_vdesc_args(
	struct xfs_inode	*ip,
	struct xfs_da_args	*args)
{
	args->geo = ip->i_mount->m_attr_geo;
	args->whichfork = XFS_ATTR_FORK,
	args->attr_filter = XFS_ATTR_VERITY;
	args->op_flags = XFS_DA_OP_OKNOENT;
	args->dp = ip;
	args->owner = ip->i_ino;
	args->name = XFS_VERITY_DESCRIPTOR_NAME;
	args->namelen = XFS_VERITY_DESCRIPTOR_NAME_LEN;
	xfs_attr_sethash(args);
}

/*
 * Initialize an args structure to load or store a merkle tree block.
 * Caller must ensure @args is zeroed except for value and valuelen.
 */
static inline void
xfs_fsverity_init_merkle_args(
	struct xfs_inode	*ip,
	struct xfs_merkle_key	*key,
	uint64_t		merkleoff,
	struct xfs_da_args	*args)
{
	xfs_merkle_key_to_disk(key, merkleoff);
	args->geo = ip->i_mount->m_attr_geo;
	args->whichfork = XFS_ATTR_FORK,
	args->attr_filter = XFS_ATTR_VERITY;
	args->op_flags = XFS_DA_OP_OKNOENT;
	args->dp = ip;
	args->owner = ip->i_ino;
	args->name = (const uint8_t *)key;
	args->namelen = sizeof(struct xfs_merkle_key);
	xfs_attr_sethash(args);
}

/* Delete the verity descriptor. */
static int
xfs_fsverity_delete_descriptor(
	struct xfs_inode	*ip)
{
	struct xfs_da_args	args = { };

	xfs_fsverity_init_vdesc_args(ip, &args);
	return xfs_attr_set(&args, XFS_ATTRUPDATE_REMOVE, false);
}

/* Delete a merkle tree block. */
static int
xfs_fsverity_delete_merkle_block(
	struct xfs_inode	*ip,
	u64			pos)
{
	struct xfs_merkle_key	name;
	struct xfs_da_args	args = { };

	xfs_fsverity_init_merkle_args(ip, &name, pos, &args);
	return xfs_attr_set(&args, XFS_ATTRUPDATE_REMOVE, false);
}

/* Retrieve the verity descriptor. */
static int
xfs_fsverity_get_descriptor(
	struct inode		*inode,
	void			*buf,
	size_t			buf_size)
{
	struct xfs_inode	*ip = XFS_I(inode);
	struct xfs_da_args	args = {
		.value		= buf,
		.valuelen	= buf_size,
	};
	int			error = 0;

	/*
	 * The fact that (returned attribute size) == (provided buf_size) is
	 * checked by xfs_attr_copy_value() (returns -ERANGE).  No descriptor
	 * is treated as a short read so that common fsverity code will
	 * complain.
	 */
	xfs_fsverity_init_vdesc_args(ip, &args);
	error = xfs_attr_get(&args);
	if (error == -ENOATTR)
		return 0;
	if (error)
		return error;

	return args.valuelen;
}

/*
 * Clear out old fsverity metadata before we start building a new one.  This
 * could happen if, say, we crashed while building fsverity data.
 */
static int
xfs_fsverity_delete_stale_metadata(
	struct xfs_inode	*ip,
	u64			new_tree_size,
	unsigned int		tree_blocksize)
{
	u64			pos;
	int			error = 0;

	/*
	 * Delete as many merkle tree blocks in increasing blkno order until we
	 * don't find any more.  That ought to be good enough for avoiding
	 * dead bloat without excessive runtime.
	 */
	for (pos = new_tree_size; !error; pos += tree_blocksize) {
		if (fatal_signal_pending(current))
			return -EINTR;
		error = xfs_fsverity_delete_merkle_block(ip, pos);
		if (error)
			break;
	}

	return error != -ENOATTR ? error : 0;
}

/* Prepare to enable fsverity by clearing old metadata. */
static int
xfs_fsverity_begin_enable(
	struct file		*filp,
	u64			merkle_tree_size,
	unsigned int		tree_blocksize)
{
	struct inode		*inode = file_inode(filp);
	struct xfs_inode	*ip = XFS_I(inode);
	int			error;

	xfs_assert_ilocked(ip, XFS_IOLOCK_EXCL);

	if (IS_DAX(inode))
		return -EINVAL;

	if (xfs_iflags_test_and_set(ip, XFS_VERITY_CONSTRUCTION))
		return -EBUSY;

	error = xfs_qm_dqattach(ip);
	if (error)
		return error;

	return xfs_fsverity_delete_stale_metadata(ip, merkle_tree_size,
			tree_blocksize);
}

/* Try to remove all the fsverity metadata after a failed enablement. */
static int
xfs_fsverity_delete_metadata(
	struct xfs_inode	*ip,
	u64			merkle_tree_size,
	unsigned int		tree_blocksize)
{
	u64			pos;
	int			error;

	if (!merkle_tree_size)
		return 0;

	for (pos = 0; pos < merkle_tree_size; pos += tree_blocksize) {
		if (fatal_signal_pending(current))
			return -EINTR;
		error = xfs_fsverity_delete_merkle_block(ip, pos);
		if (error == -ENOATTR)
			error = 0;
		if (error)
			return error;
	}

	error = xfs_fsverity_delete_descriptor(ip);
	return error != -ENOATTR ? error : 0;
}

/* Complete (or fail) the process of enabling fsverity. */
static int
xfs_fsverity_end_enable(
	struct file		*filp,
	const void		*desc,
	size_t			desc_size,
	u64			merkle_tree_size,
	unsigned int		tree_blocksize)
{
	struct xfs_da_args	args = {
		.value		= (void *)desc,
		.valuelen	= desc_size,
	};
	struct inode		*inode = file_inode(filp);
	struct xfs_inode	*ip = XFS_I(inode);
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	int			error = 0;

	xfs_assert_ilocked(ip, XFS_IOLOCK_EXCL);

	/* fs-verity failed, just cleanup */
	if (desc == NULL)
		goto out;

	xfs_fsverity_init_vdesc_args(ip, &args);
	error = xfs_attr_set(&args, XFS_ATTRUPDATE_UPSERT, false);
	if (error)
		goto out;

	/* Set fsverity inode flag */
	error = xfs_trans_alloc_inode(ip, &M_RES(mp)->tr_ichange,
			0, 0, false, &tp);
	if (error)
		goto out;

	/*
	 * Ensure that we've persisted the verity information before we enable
	 * it on the inode and tell the caller we have sealed the inode.
	 */
	ip->i_diflags2 |= XFS_DIFLAG2_VERITY;

	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	xfs_trans_set_sync(tp);

	error = xfs_trans_commit(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);

	if (!error)
		inode->i_flags |= S_VERITY;

out:
	if (error) {
		int	error2;

		error2 = xfs_fsverity_delete_metadata(ip,
				merkle_tree_size, tree_blocksize);
		if (error2)
			xfs_alert(ip->i_mount,
 "ino 0x%llx failed to clean up new fsverity metadata, err %d",
					ip->i_ino, error2);
	}

	xfs_iflags_clear(ip, XFS_VERITY_CONSTRUCTION);
	return error;
}

/* Retrieve a merkle tree block. */
static int
xfs_fsverity_read_merkle(
	const struct fsverity_readmerkle *req,
	struct fsverity_blockbuf	*block)
{
	struct xfs_inode		*ip = XFS_I(req->inode);
	struct xfs_merkle_key		name;
	struct xfs_da_args		args = {
		.valuelen		= block->size,
	};
	struct xfs_merkle_blob		*mk, *new_mk;
	int				error;

	/* Is the block already cached? */
	mk = xfs_fsverity_cache_load(ip, block->pos);
	if (mk)
		goto out_hit;

	new_mk = xfs_merkle_blob_alloc(ip, block->pos, block->size);
	if (!new_mk)
		return -ENOMEM;
	args.value = new_mk->data;

	/* Read the block in from disk and try to store it in the cache. */
	xfs_fsverity_init_merkle_args(ip, &name, block->pos, &args);
	error = xfs_attr_get(&args);
	if (error)
		goto out_new_mk;

	mk = xfs_fsverity_cache_store(ip, new_mk);
	if (IS_ERR(mk)) {
		xfs_merkle_blob_rele(new_mk);
		return PTR_ERR(mk);
	}
	if (mk != new_mk) {
		/*
		 * We raced with another thread to populate the cache and lost.
		 * Free the new cache blob and continue with the existing one.
		 */
		xfs_merkle_blob_rele(new_mk);
	}

out_hit:
	block->kaddr   = (void *)mk->data;
	block->context = mk;
	block->verified = test_bit(XFS_MERKLE_BLOB_VERIFIED_BIT, &mk->flags);

	return 0;

out_new_mk:
	xfs_merkle_blob_rele(new_mk);
	return error;
}

/* Write a merkle tree block. */
static int
xfs_fsverity_write_merkle(
	const struct fsverity_writemerkle *req,
	const void			*buf,
	u64				pos,
	unsigned int			size)
{
	struct inode			*inode = req->inode;
	struct xfs_inode		*ip = XFS_I(inode);
	struct xfs_merkle_key		name;
	struct xfs_da_args		args = {
		.value			= (void *)buf,
		.valuelen		= size,
	};

	xfs_fsverity_init_merkle_args(ip, &name, pos, &args);
	return xfs_attr_set(&args, XFS_ATTRUPDATE_UPSERT, false);
}

/* Drop a cached merkle tree block.. */
static void
xfs_fsverity_drop_merkle(
	struct fsverity_blockbuf	*block)
{
	struct xfs_merkle_blob		*mk = block->context;

	if (block->verified)
		set_bit(XFS_MERKLE_BLOB_VERIFIED_BIT, &mk->flags);
	xfs_merkle_blob_rele(mk);
	block->kaddr = NULL;
	block->context = NULL;
}

const struct fsverity_operations xfs_fsverity_ops = {
	.begin_enable_verity		= xfs_fsverity_begin_enable,
	.end_enable_verity		= xfs_fsverity_end_enable,
	.get_verity_descriptor		= xfs_fsverity_get_descriptor,
	.read_merkle_tree_block		= xfs_fsverity_read_merkle,
	.write_merkle_tree_block	= xfs_fsverity_write_merkle,
	.drop_merkle_tree_block		= xfs_fsverity_drop_merkle,
};
