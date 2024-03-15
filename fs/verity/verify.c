// SPDX-License-Identifier: GPL-2.0
/*
 * Data verification functions, i.e. hooks for ->readahead()
 *
 * Copyright 2019 Google LLC
 */

#include "fsverity_private.h"

#include <crypto/hash.h>
#include <linux/bio.h>

static struct workqueue_struct *fsverity_read_workqueue;

/*
 * Returns true if the hash block with index @hblock_idx in the tree has
 * already been verified.
 */
static bool is_hash_block_verified(struct inode *inode,
				   struct fsverity_blockbuf *block,
				   unsigned long hblock_idx)
{
	unsigned int blocks_per_page;
	unsigned int i;
	struct fsverity_info *vi = inode->i_verity_info;
	struct page *hpage;

	/*
	 * If the filesystem uses block-based caching, then rely on the
	 * implementation to retain verified status.
	 */
	if (fsverity_caches_blocks(inode))
		return block->verified;

	/* Otherwise, the filesystem uses page-based caching. */
	hpage = (struct page *)block->context;

	/*
	 * When the Merkle tree block size and page size are the same, then the
	 * ->hash_block_verified bitmap isn't allocated, and we use PG_checked
	 * to directly indicate whether the page's block has been verified.
	 *
	 * Using PG_checked also guarantees that we re-verify hash pages that
	 * get evicted and re-instantiated from the backing storage, as new
	 * pages always start out with PG_checked cleared.
	 */
	if (!fsverity_uses_bitmap(vi, inode))
		return PageChecked(hpage);

	/*
	 * When the Merkle tree block size and page size differ, we use a bitmap
	 * to indicate whether each hash block has been verified.
	 *
	 * However, we still need to ensure that hash pages that get evicted and
	 * re-instantiated from the backing storage are re-verified.  To do
	 * this, we use PG_checked again, but now it doesn't really mean
	 * "checked".  Instead, now it just serves as an indicator for whether
	 * the hash page is newly instantiated or not.  If the page is new, as
	 * indicated by PG_checked=0, we clear the bitmap bits for the page's
	 * blocks since they are untrustworthy, then set PG_checked=1.
	 * Otherwise we return the bitmap bit for the requested block.
	 *
	 * Multiple threads may execute this code concurrently on the same page.
	 * This is safe because we use memory barriers to ensure that if a
	 * thread sees PG_checked=1, then it also sees the associated bitmap
	 * clearing to have occurred.  Also, all writes and their corresponding
	 * reads are atomic, and all writes are safe to repeat in the event that
	 * multiple threads get into the PG_checked=0 section.  (Clearing a
	 * bitmap bit again at worst causes a hash block to be verified
	 * redundantly.  That event should be very rare, so it's not worth using
	 * a lock to avoid.  Setting PG_checked again has no effect.)
	 */
	if (PageChecked(hpage)) {
		/*
		 * A read memory barrier is needed here to give ACQUIRE
		 * semantics to the above PageChecked() test.
		 */
		smp_rmb();
		return test_bit(hblock_idx, vi->hash_block_verified);
	}
	blocks_per_page = vi->tree_params.blocks_per_page;
	hblock_idx = round_down(hblock_idx, blocks_per_page);
	for (i = 0; i < blocks_per_page; i++)
		clear_bit(hblock_idx + i, vi->hash_block_verified);
	/*
	 * A write memory barrier is needed here to give RELEASE semantics to
	 * the below SetPageChecked() operation.
	 */
	smp_wmb();
	SetPageChecked(hpage);
	return false;
}

/*
 * Verify a single data block against the file's Merkle tree.
 *
 * In principle, we need to verify the entire path to the root node.  However,
 * for efficiency the filesystem may cache the hash blocks.  Therefore we need
 * only ascend the tree until an already-verified hash block is seen, and then
 * verify the path to that block.
 *
 * Return: %true if the data block is valid, else %false.
 */
static bool
verify_data_block(struct inode *inode, struct fsverity_info *vi,
		  const void *data, u64 data_pos, unsigned long max_ra_bytes)
{
	const struct merkle_tree_params *params = &vi->tree_params;
	const unsigned int hsize = params->digest_size;
	int level;
	int err = 0;
	unsigned long ra_bytes;
	u8 _want_hash[FS_VERITY_MAX_DIGEST_SIZE];
	const u8 *want_hash;
	u8 real_hash[FS_VERITY_MAX_DIGEST_SIZE];
	/* The hash blocks that are traversed, indexed by level */
	struct {
		/* Buffer containing the hash block */
		struct fsverity_blockbuf block;
		/* Index of the hash block in the tree overall */
		unsigned long index;
		/* Byte offset of the wanted hash relative to @addr */
		unsigned int hoffset;
	} hblocks[FS_VERITY_MAX_LEVELS];
	trace_fsverity_verify_block(inode, data_pos);
	/*
	 * The index of the previous level's block within that level; also the
	 * index of that block's hash within the current level.
	 */
	u64 hidx = data_pos >> params->log_blocksize;

	/* Up to 1 + FS_VERITY_MAX_LEVELS pages may be mapped at once */
	BUILD_BUG_ON(1 + FS_VERITY_MAX_LEVELS > KM_MAX_IDX);

	if (unlikely(data_pos >= inode->i_size)) {
		/*
		 * This can happen in the data page spanning EOF when the Merkle
		 * tree block size is less than the page size.  The Merkle tree
		 * doesn't cover data blocks fully past EOF.  But the entire
		 * page spanning EOF can be visible to userspace via a mmap, and
		 * any part past EOF should be all zeroes.  Therefore, we need
		 * to verify that any data blocks fully past EOF are all zeroes.
		 */
		if (memchr_inv(data, 0, params->block_size)) {
			fsverity_err(inode,
				     "FILE CORRUPTED!  Data past EOF is not zeroed");
			return false;
		}
		return true;
	}

	/*
	 * Starting at the leaf level, ascend the tree saving hash blocks along
	 * the way until we find a hash block that has already been verified, or
	 * until we reach the root.
	 */
	for (level = 0; level < params->num_levels; level++) {
		unsigned long next_hidx;
		unsigned long hblock_idx;
		u64 hblock_pos;
		unsigned int hoffset;
		struct fsverity_blockbuf *block = &hblocks[level].block;

		/*
		 * The index of the block in the current level; also the index
		 * of that block's hash within the next level.
		 */
		next_hidx = hidx >> params->log_arity;

		/* Index of the hash block in the tree overall */
		hblock_idx = params->level_start[level] + next_hidx;

		/* Byte offset of the hash block in the tree overall */
		hblock_pos = hblock_idx << params->log_blocksize;

		/* Byte offset of the hash within the block */
		hoffset = (hidx << params->log_digestsize) &
			  (params->block_size - 1);

		if (level == 0)
			ra_bytes = min(max_ra_bytes,
				       params->tree_size - hblock_pos);
		else
			ra_bytes = 0;

		err = fsverity_read_merkle_tree_block(inode, params, level,
				hblock_pos, ra_bytes, block);
		if (err) {
			fsverity_err(inode,
				     "Error %d reading Merkle tree block %llu",
				     err, hblock_pos);
			goto error;
		}

		if (is_hash_block_verified(inode, block, hblock_idx)) {
			memcpy(_want_hash, block->kaddr + hoffset, hsize);
			want_hash = _want_hash;
			trace_fsverity_merkle_tree_block_verified(inode,
					block, FSVERITY_TRACE_DIR_ASCEND);
			fsverity_drop_merkle_tree_block(inode, block);
			goto descend;
		}
		hblocks[level].index = hblock_idx;
		hblocks[level].hoffset = hoffset;
		hidx = next_hidx;
	}

	want_hash = vi->root_hash;
descend:
	/* Descend the tree verifying hash blocks. */
	for (; level > 0; level--) {
		struct fsverity_blockbuf *block = &hblocks[level - 1].block;
		const void *haddr = block->kaddr;
		unsigned long hblock_idx = hblocks[level - 1].index;
		unsigned int hoffset = hblocks[level - 1].hoffset;

		if (fsverity_hash_block(params, inode, haddr, real_hash) != 0)
			goto error;
		if (memcmp(want_hash, real_hash, hsize) != 0)
			goto corrupted;
		/*
		 * Mark the hash block as verified.  This must be atomic and
		 * idempotent, as the same hash block might be verified by
		 * multiple threads concurrently.
		 */
		if (fsverity_caches_blocks(inode))
			block->verified = true;
		else if (fsverity_uses_bitmap(vi, inode))
			set_bit(hblock_idx, vi->hash_block_verified);
		else
			SetPageChecked((struct page *)block->context);
		memcpy(_want_hash, haddr + hoffset, hsize);
		want_hash = _want_hash;
		trace_fsverity_merkle_tree_block_verified(inode, block,
				FSVERITY_TRACE_DIR_DESCEND);
		fsverity_drop_merkle_tree_block(inode, block);
	}

	/* Finally, verify the data block. */
	if (fsverity_hash_block(params, inode, data, real_hash) != 0)
		goto error;
	if (memcmp(want_hash, real_hash, hsize) != 0)
		goto corrupted;
	return true;

corrupted:
	fsverity_err(inode,
		     "FILE CORRUPTED! pos=%llu, level=%d, want_hash=%s:%*phN, real_hash=%s:%*phN",
		     data_pos, level - 1,
		     params->hash_alg->name, hsize, want_hash,
		     params->hash_alg->name, hsize, real_hash);
error:
	for (; level > 0; level--)
		fsverity_drop_merkle_tree_block(inode, &hblocks[level - 1].block);
	return false;
}

static void fsverity_fail_validation(struct inode *inode, loff_t pos,
				     size_t len)
{
	const struct fsverity_operations *vops = inode->i_sb->s_vop;

	if (vops->fail_validation)
		vops->fail_validation(inode, pos, len);
}

static bool
verify_data_blocks(struct folio *data_folio, size_t len, size_t offset,
		   unsigned long max_ra_bytes)
{
	struct inode *inode = data_folio->mapping->host;
	struct fsverity_info *vi = inode->i_verity_info;
	const unsigned int block_size = vi->tree_params.block_size;
	u64 pos = (u64)data_folio->index << PAGE_SHIFT;

	if (WARN_ON_ONCE(len <= 0 || !IS_ALIGNED(len | offset, block_size)))
		return false;
	if (WARN_ON_ONCE(!folio_test_locked(data_folio) ||
			 folio_test_uptodate(data_folio)))
		return false;
	do {
		void *data;
		bool valid;

		data = kmap_local_folio(data_folio, offset);
		valid = verify_data_block(inode, vi, data, pos + offset,
					  max_ra_bytes);
		kunmap_local(data);
		if (!valid) {
			fsverity_fail_validation(inode, pos + offset,
						 block_size);
			return false;
		}
		offset += block_size;
		len -= block_size;
	} while (len);
	return true;
}

/**
 * fsverity_verify_blocks() - verify data in a folio
 * @folio: the folio containing the data to verify
 * @len: the length of the data to verify in the folio
 * @offset: the offset of the data to verify in the folio
 *
 * Verify data that has just been read from a verity file.  The data must be
 * located in a pagecache folio that is still locked and not yet uptodate.  The
 * length and offset of the data must be Merkle tree block size aligned.
 *
 * Return: %true if the data is valid, else %false.
 */
bool fsverity_verify_blocks(struct folio *folio, size_t len, size_t offset)
{
	return verify_data_blocks(folio, len, offset, 0);
}
EXPORT_SYMBOL_GPL(fsverity_verify_blocks);

#ifdef CONFIG_BLOCK
/**
 * fsverity_verify_bio() - verify a 'read' bio that has just completed
 * @bio: the bio to verify
 *
 * Verify the bio's data against the file's Merkle tree.  All bio data segments
 * must be aligned to the file's Merkle tree block size.  If any data fails
 * verification, then bio->bi_status is set to an error status.
 *
 * This is a helper function for use by the ->readahead() method of filesystems
 * that issue bios to read data directly into the page cache.  Filesystems that
 * populate the page cache without issuing bios (e.g. non block-based
 * filesystems) must instead call fsverity_verify_page() directly on each page.
 * All filesystems must also call fsverity_verify_page() on holes.
 */
void fsverity_verify_bio(struct bio *bio)
{
	struct folio_iter fi;
	unsigned long max_ra_pages = 0;

	if (bio->bi_opf & REQ_RAHEAD) {
		/*
		 * If this bio is for data readahead, then we also do readahead
		 * of the first (largest) level of the Merkle tree.  Namely,
		 * when a Merkle tree page is read, we also try to piggy-back on
		 * some additional pages -- up to 1/4 the number of data pages.
		 *
		 * This improves sequential read performance, as it greatly
		 * reduces the number of I/O requests made to the Merkle tree.
		 */
		max_ra_pages = bio->bi_iter.bi_size >> (PAGE_SHIFT + 2);
	}

	bio_for_each_folio_all(fi, bio) {
		if (!verify_data_blocks(fi.folio, fi.length, fi.offset,
					max_ra_pages << PAGE_SHIFT)) {
			bio->bi_status = BLK_STS_IOERR;
			break;
		}
	}
}
EXPORT_SYMBOL_GPL(fsverity_verify_bio);
#endif /* CONFIG_BLOCK */

/**
 * fsverity_enqueue_verify_work() - enqueue work on the fs-verity workqueue
 * @work: the work to enqueue
 *
 * Enqueue verification work for asynchronous processing.
 */
void fsverity_enqueue_verify_work(struct work_struct *work)
{
	queue_work(fsverity_read_workqueue, work);
}
EXPORT_SYMBOL_GPL(fsverity_enqueue_verify_work);

void __init fsverity_init_workqueue(void)
{
	/*
	 * Use a high-priority workqueue to prioritize verification work, which
	 * blocks reads from completing, over regular application tasks.
	 *
	 * For performance reasons, don't use an unbound workqueue.  Using an
	 * unbound workqueue for crypto operations causes excessive scheduler
	 * latency on ARM64.
	 */
	fsverity_read_workqueue = alloc_workqueue("fsverity_read_queue",
						  WQ_HIGHPRI,
						  num_online_cpus());
	if (!fsverity_read_workqueue)
		panic("failed to allocate fsverity_read_queue");
}

/**
 * fsverity_read_merkle_tree_block() - read Merkle tree block
 * @inode: inode to which this Merkle tree blocks belong
 * @params: merkle tree parameters
 * @level: expected level of the block; level 0 are the leaves, -1 means a
 * streaming read
 * @pos: byte position within merkle tree
 * @ra_bytes: try to read ahead this many btes
 * @block: block to be loaded
 *
 * This function loads data from a merkle tree.
 */
int fsverity_read_merkle_tree_block(struct inode *inode,
				    const struct merkle_tree_params *params,
				    int level, u64 pos, unsigned long ra_bytes,
				    struct fsverity_blockbuf *block)
{
	const struct fsverity_operations *vops = inode->i_sb->s_vop;
	unsigned long page_idx;
	struct page *page;
	unsigned long index;
	unsigned int offset_in_page;

	block->offset = pos;
	block->size = params->block_size;

	if (fsverity_caches_blocks(inode)) {
		struct fsverity_readmerkle req = {
			.inode = inode,
			.level = level,
			.num_levels = params->num_levels,
			.log_blocksize = params->log_blocksize,
			.ra_bytes = ra_bytes,
			.zero_digest = params->zero_digest,
			.digest_size = params->digest_size,
		};
		block->verified = false;

		return vops->read_merkle_tree_block(&req, block);
	}

	index = pos >> params->log_blocksize;
	page_idx = round_down(index, params->blocks_per_page);
	offset_in_page = pos & ~PAGE_MASK;

	page = vops->read_merkle_tree_page(inode, page_idx,
			ra_bytes >> PAGE_SHIFT);
	if (IS_ERR(page))
		return PTR_ERR(page);

	block->kaddr = kmap_local_page(page) + offset_in_page;
	block->context = page;
	return 0;
}

void fsverity_drop_merkle_tree_block(struct inode *inode,
				     struct fsverity_blockbuf *block)
{
	if (fsverity_caches_blocks(inode))  {
		inode->i_sb->s_vop->drop_merkle_tree_block(block);
	} else {
		kunmap_local(block->kaddr);
		put_page((struct page *)block->context);
	}
	block->kaddr = NULL;
	block->context = NULL;
}
