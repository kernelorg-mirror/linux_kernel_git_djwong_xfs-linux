// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_rmap_btree.h"
#include "xfs_alloc.h"
#include "xfs_ialloc.h"
#include "xfs_rmap.h"
#include "xfs_ag.h"
#include "xfs_ag_resv.h"
#include "xfs_health.h"
#include "xfs_error.h"
#include "xfs_bmap.h"
#include "xfs_defer.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_trace.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_buf_item.h"
#include "xfs_rtgroup.h"
#include "xfs_rtbitmap.h"

/*
 * Passive reference counting access wrappers to the rtgroup structures.  If
 * the rtgroup structure is to be freed, the freeing code is responsible for
 * cleaning up objects with passive references before freeing the structure.
 */
struct xfs_rtgroup *
xfs_rtgroup_get(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	struct xfs_rtgroup	*rtg;
	int			ref = 0;

	rcu_read_lock();
	rtg = radix_tree_lookup(&mp->m_rtgroup_tree, rgno);
	if (rtg) {
		ASSERT(atomic_read(&rtg->rtg_ref) >= 0);
		ref = atomic_inc_return(&rtg->rtg_ref);
	}
	rcu_read_unlock();
	trace_xfs_rtgroup_get(mp, rgno, ref, _RET_IP_);
	return rtg;
}

struct xfs_rtgroup *
xfs_rtgroup_bump(
	struct xfs_rtgroup	*rtg)
{
	if (!atomic_inc_not_zero(&rtg->rtg_ref)) {
		ASSERT(0);
		return NULL;
	}

	trace_xfs_rtgroup_bump(rtg->rtg_mount, rtg->rtg_rgno,
			atomic_read(&rtg->rtg_ref), _RET_IP_);
	return rtg;
}

void
xfs_rtgroup_put(
	struct xfs_rtgroup	*rtg)
{
	int			ref;

	ASSERT(atomic_read(&rtg->rtg_ref) > 0);
	ref = atomic_dec_return(&rtg->rtg_ref);
	trace_xfs_rtgroup_put(rtg->rtg_mount, rtg->rtg_rgno, ref, _RET_IP_);
}

int
xfs_initialize_rtgroups(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgcount)
{
	struct xfs_rtgroup	*rtg;
	xfs_rgnumber_t		index;
	xfs_rgnumber_t		first_initialised = NULLRGNUMBER;
	int			error;

	if (!xfs_has_rtgroups(mp))
		return 0;

	/*
	 * Walk the current rtgroup tree so we don't try to initialise rt
	 * groups that already exist (growfs case). Allocate and insert all the
	 * rtgroups we don't find ready for initialisation.
	 */
	for (index = 0; index < rgcount; index++) {
		rtg = xfs_rtgroup_get(mp, index);
		if (rtg) {
			xfs_rtgroup_put(rtg);
			continue;
		}

		rtg = kmem_zalloc(sizeof(struct xfs_rtgroup), KM_MAYFAIL);
		if (!rtg) {
			error = -ENOMEM;
			goto out_unwind_new_rtgs;
		}
		rtg->rtg_rgno = index;
		rtg->rtg_mount = mp;

		error = radix_tree_preload(GFP_NOFS);
		if (error)
			goto out_free_rtg;

		spin_lock(&mp->m_rtgroup_lock);
		if (radix_tree_insert(&mp->m_rtgroup_tree, index, rtg)) {
			WARN_ON_ONCE(1);
			spin_unlock(&mp->m_rtgroup_lock);
			radix_tree_preload_end();
			error = -EEXIST;
			goto out_free_rtg;
		}
		spin_unlock(&mp->m_rtgroup_lock);
		radix_tree_preload_end();

#ifdef __KERNEL__
		/* Place kernel structure only init below this point. */
		spin_lock_init(&rtg->rtg_state_lock);
#endif /* __KERNEL__ */

		/* first new rtg is fully initialized */
		if (first_initialised == NULLRGNUMBER)
			first_initialised = index;
	}

	return 0;

out_free_rtg:
	kmem_free(rtg);
out_unwind_new_rtgs:
	/* unwind any prior newly initialized rtgs */
	for (index = first_initialised; index < rgcount; index++) {
		rtg = radix_tree_delete(&mp->m_rtgroup_tree, index);
		if (!rtg)
			break;
		kmem_free(rtg);
	}
	return error;
}

STATIC void
__xfs_free_rtgroups(
	struct rcu_head		*head)
{
	struct xfs_rtgroup	*rtg;

	rtg = container_of(head, struct xfs_rtgroup, rcu_head);
	kmem_free(rtg);
}

/*
 * Free up the rtgroup resources associated with the mount structure.
 */
void
xfs_free_rtgroups(
	struct xfs_mount	*mp)
{
	struct xfs_rtgroup	*rtg;
	xfs_rgnumber_t		rgno;

	if (!xfs_has_rtgroups(mp))
		return;

	for (rgno = 0; rgno < mp->m_sb.sb_rgcount; rgno++) {
		spin_lock(&mp->m_rtgroup_lock);
		rtg = radix_tree_delete(&mp->m_rtgroup_tree, rgno);
		spin_unlock(&mp->m_rtgroup_lock);
		ASSERT(rtg);
		XFS_IS_CORRUPT(rtg->rtg_mount, atomic_read(&rtg->rtg_ref) != 0);

		call_rcu(&rtg->rcu_head, __xfs_free_rtgroups);
	}
}

/* Find the size of the rtgroup, in blocks. */
static xfs_rgblock_t
__xfs_rtgroup_block_count(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno,
	xfs_rgnumber_t		rgcount,
	xfs_rfsblock_t		rblocks)
{
	ASSERT(rgno < rgcount);

	if (rgno < rgcount - 1)
		return mp->m_sb.sb_rgblocks;
	return xfs_rtb_rounddown_rtx(mp,
			rblocks - (rgno * mp->m_sb.sb_rgblocks));
}

/* Compute the number of blocks in this realtime group. */
xfs_rgblock_t
xfs_rtgroup_block_count(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	return __xfs_rtgroup_block_count(mp, rgno, mp->m_sb.sb_rgcount,
			mp->m_sb.sb_rblocks);
}

static xfs_failaddr_t
xfs_rtsb_verify(
	struct xfs_buf		*bp)
{
	struct xfs_mount	*mp = bp->b_mount;
	struct xfs_rtsb		*rsb = bp->b_addr;

	if (!xfs_verify_magic(bp, rsb->rsb_magicnum))
		return __this_address;
	if (be32_to_cpu(rsb->rsb_blocksize) != mp->m_sb.sb_blocksize)
		return __this_address;
	if (be64_to_cpu(rsb->rsb_rblocks) != mp->m_sb.sb_rblocks)
		return __this_address;

	if (be64_to_cpu(rsb->rsb_rextents) != mp->m_sb.sb_rextents)
		return __this_address;

	if (!uuid_equal(&rsb->rsb_uuid, &mp->m_sb.sb_uuid))
		return __this_address;

	if (be32_to_cpu(rsb->rsb_rgcount) != mp->m_sb.sb_rgcount)
		return __this_address;

	if (be32_to_cpu(rsb->rsb_rextsize) != mp->m_sb.sb_rextsize)
		return __this_address;
	if (be32_to_cpu(rsb->rsb_rbmblocks) != mp->m_sb.sb_rbmblocks)
		return __this_address;

	if (be32_to_cpu(rsb->rsb_rgblocks) != mp->m_sb.sb_rgblocks)
		return __this_address;
	if (rsb->rsb_blocklog != mp->m_sb.sb_blocklog)
		return __this_address;
	if (rsb->rsb_sectlog != mp->m_sb.sb_sectlog)
		return __this_address;
	if (rsb->rsb_rextslog != mp->m_sb.sb_rextslog)
		return __this_address;
	if (rsb->rsb_pad)
		return __this_address;

	if (rsb->rsb_pad2)
		return __this_address;

	if (!uuid_equal(&rsb->rsb_meta_uuid, &mp->m_sb.sb_meta_uuid))
		return __this_address;

	/* Everything to the end of the fs block must be zero */
	if (memchr_inv(rsb + 1, 0, BBTOB(bp->b_length) - sizeof(*rsb)))
		return __this_address;

	return NULL;
}

static void
xfs_rtsb_read_verify(
	struct xfs_buf	*bp)
{
	xfs_failaddr_t	fa;

	if (!xfs_buf_verify_cksum(bp, XFS_RTSB_CRC_OFF))
		xfs_verifier_error(bp, -EFSBADCRC, __this_address);
	else {
		fa = xfs_rtsb_verify(bp);
		if (fa)
			xfs_verifier_error(bp, -EFSCORRUPTED, fa);
	}
}

static void
xfs_rtsb_write_verify(
	struct xfs_buf		*bp)
{
	struct xfs_rtsb		*rsb = bp->b_addr;
	struct xfs_buf_log_item	*bip = bp->b_log_item;
	xfs_failaddr_t		fa;

	fa = xfs_rtsb_verify(bp);
	if (fa) {
		xfs_verifier_error(bp, -EFSCORRUPTED, fa);
		return;
	}

	if (bip)
		rsb->rsb_lsn = cpu_to_be64(bip->bli_item.li_lsn);

	xfs_buf_update_cksum(bp, XFS_RTSB_CRC_OFF);
}

const struct xfs_buf_ops xfs_rtsb_buf_ops = {
	.name = "xfs_rtsb",
	.magic = { 0, cpu_to_be32(XFS_RTSB_MAGIC) },
	.verify_read = xfs_rtsb_read_verify,
	.verify_write = xfs_rtsb_write_verify,
	.verify_struct = xfs_rtsb_verify,
};
