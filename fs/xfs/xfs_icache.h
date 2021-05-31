// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef XFS_SYNC_H
#define XFS_SYNC_H 1

struct xfs_mount;
struct xfs_perag;

struct xfs_icwalk {
	__u32		icw_flags;
	kuid_t		icw_uid;
	kgid_t		icw_gid;
	prid_t		icw_prid;
	__u64		icw_min_file_size;
	int		icw_scan_limit;
};

/* Flags that we borrowed from struct xfs_fs_eofblocks */
#define XFS_ICWALK_FLAG_SYNC		(XFS_EOF_FLAGS_SYNC)
#define XFS_ICWALK_FLAG_UID		(XFS_EOF_FLAGS_UID)
#define XFS_ICWALK_FLAG_GID		(XFS_EOF_FLAGS_GID)
#define XFS_ICWALK_FLAG_PRID		(XFS_EOF_FLAGS_PRID)
#define XFS_ICWALK_FLAG_MINFILESIZE	(XFS_EOF_FLAGS_MINFILESIZE)
#define XFS_ICWALK_FLAG_UNION		(XFS_EOF_FLAGS_UNION)

#define XFS_ICWALK_FLAGS_VALID		(XFS_ICWALK_FLAG_SYNC | \
					 XFS_ICWALK_FLAG_UID | \
					 XFS_ICWALK_FLAG_GID | \
					 XFS_ICWALK_FLAG_PRID | \
					 XFS_ICWALK_FLAG_MINFILESIZE | \
					 XFS_ICWALK_FLAG_UNION)

/*
 * Flags for xfs_iget()
 */
#define XFS_IGET_CREATE		0x1
#define XFS_IGET_UNTRUSTED	0x2
#define XFS_IGET_DONTCACHE	0x4
#define XFS_IGET_INCORE		0x8	/* don't read from disk or reinit */

int xfs_iget(struct xfs_mount *mp, struct xfs_trans *tp, xfs_ino_t ino,
	     uint flags, uint lock_flags, xfs_inode_t **ipp);

/* recovery needs direct inode allocation capability */
struct xfs_inode * xfs_inode_alloc(struct xfs_mount *mp, xfs_ino_t ino);
void xfs_inode_free(struct xfs_inode *ip);

void xfs_reclaim_worker(struct work_struct *work);

void xfs_reclaim_inodes(struct xfs_mount *mp);
int xfs_reclaim_inodes_count(struct xfs_mount *mp);
long xfs_reclaim_inodes_nr(struct xfs_mount *mp, int nr_to_scan);

void xfs_inode_mark_reclaimable(struct xfs_inode *ip, bool need_inactive);

int xfs_blockgc_free_dquots(struct xfs_mount *mp, struct xfs_dquot *udqp,
		struct xfs_dquot *gdqp, struct xfs_dquot *pdqp,
		unsigned int iwalk_flags);
int xfs_blockgc_free_quota(struct xfs_inode *ip, unsigned int iwalk_flags);
int xfs_blockgc_free_space(struct xfs_mount *mp, struct xfs_icwalk *icm);

void xfs_inode_set_eofblocks_tag(struct xfs_inode *ip);
void xfs_inode_clear_eofblocks_tag(struct xfs_inode *ip);

void xfs_inode_set_cowblocks_tag(struct xfs_inode *ip);
void xfs_inode_clear_cowblocks_tag(struct xfs_inode *ip);

void xfs_blockgc_worker(struct work_struct *work);

#ifdef CONFIG_XFS_QUOTA
int xfs_dqrele_all_inodes(struct xfs_mount *mp, unsigned int qflags);
#else
# define xfs_dqrele_all_inodes(mp, qflags)	(0)
#endif

int xfs_icache_inode_is_allocated(struct xfs_mount *mp, struct xfs_trans *tp,
				  xfs_ino_t ino, bool *inuse);

void xfs_blockgc_stop(struct xfs_mount *mp);
void xfs_blockgc_start(struct xfs_mount *mp);

void xfs_inodegc_worker(struct work_struct *work);
void xfs_inodegc_flush(struct xfs_mount *mp);
void xfs_inodegc_stop(struct xfs_mount *mp);
void xfs_inodegc_start(struct xfs_mount *mp);
int xfs_inodegc_free_space(struct xfs_mount *mp, struct xfs_icwalk *icw);
bool xfs_has_inodegc_work(struct xfs_mount *mp);

/*
 * Process all pending inode inactivations immediately (sort of) so that a
 * resource usage report will be mostly accurate with regards to files that
 * have been unlinked recently.
 *
 * It isn't practical to maintain a count of the resources used by unlinked
 * inodes to adjust the values reported by this function.  Resources that are
 * shared (e.g. reflink) when an inode is queued for inactivation cannot be
 * counted towards the adjustment, and cross referencing data extents with the
 * refcount btree is the only way to decide if a resource is shared.  Worse,
 * unsharing of any data blocks in the system requires either a second
 * consultation with the refcount btree, or training users to deal with the
 * free space counts possibly fluctuating upwards as inactivations occur.
 *
 * Hence we guard the inactivation flush with a ratelimiter so that the counts
 * are not way out of whack while ignoring workloads that hammer us with statfs
 * calls.  Once per clock tick seems frequent enough to avoid complaints about
 * inaccurate counts.
 */
static inline void
xfs_inodegc_summary_flush(
	struct xfs_mount	*mp)
{
	if (__ratelimit(&mp->m_inodegc_ratelimit))
		xfs_inodegc_flush(mp);
}

#endif
