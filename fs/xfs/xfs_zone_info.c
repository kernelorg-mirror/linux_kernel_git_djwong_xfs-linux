// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023-2024 Christoph Hellwig.
 * Copyright (c) 2024, Western Digital Corporation or its affiliates.
 */
#include "xfs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_rtgroup.h"
#include "xfs_zone_alloc.h"
#include "xfs_zone_priv.h"

static const char xfs_write_hint_shorthand[6][16] = {
	"NOT_SET", "NONE", "SHORT", "MEDIUM", "LONG", "EXTREME"};

static inline const char *
xfs_write_hint_to_str(
	uint8_t			write_hint)
{
	if (write_hint > WRITE_LIFE_EXTREME)
		return "UNKNOWN";
	return xfs_write_hint_shorthand[write_hint];
}

static void
xfs_show_open_zone(
	struct seq_file		*m,
	struct xfs_open_zone	*oz)
{
	seq_printf(m, "\t  zone %d, wp %u, written %u, used %u, hint %s\n",
		rtg_rgno(oz->oz_rtg),
		oz->oz_write_pointer, oz->oz_written,
		rtg_rmap(oz->oz_rtg)->i_used_blocks,
		xfs_write_hint_to_str(oz->oz_write_hint));
}

#define XFS_USED_BUCKETS	10
#define XFS_USED_BUCKET_PCT	(100 / XFS_USED_BUCKETS)

static unsigned int
xfs_zone_to_bucket(
	struct xfs_rtgroup      *rtg)
{
	return div_u64(rtg_rmap(rtg)->i_used_blocks * XFS_USED_BUCKETS,
			rtg->rtg_extents);
}

static void
xfs_show_full_zone_used_distribution(
	struct seq_file         *m,
	struct xfs_mount        *mp)
{
	struct xfs_zone_info	*zi = mp->m_zone_info;
	XA_STATE		(xas, &mp->m_groups[XG_TYPE_RTG].xa, 0);
	unsigned int            buckets[XFS_USED_BUCKETS] = {0};
	unsigned int		reclaimable = 0, full, i;
	struct xfs_rtgroup      *rtg;

	lockdep_assert_held(&zi->zi_zone_list_lock);

	rcu_read_lock();
	xas_for_each_marked(&xas, rtg, ULONG_MAX, XFS_RTG_RECLAIMABLE) {
		buckets[xfs_zone_to_bucket(rtg)]++;
		reclaimable++;
	}
	rcu_read_unlock();

	for (i = 0; i < XFS_USED_BUCKETS; i++)
		seq_printf(m, "\t  %2u..%2u%%: %u\n", i * XFS_USED_BUCKET_PCT,
				(i + 1) * XFS_USED_BUCKET_PCT - 1, buckets[i]);

	full = mp->m_sb.sb_rgcount;
	if (zi->zi_open_gc_zone)
		full--;
	full -= zi->zi_nr_open_zones;
	full -= atomic_read(&zi->zi_nr_free_zones);
	full -= reclaimable;

	seq_printf(m, "\t     100%%: %u\n", full);
}

void
xfs_zoned_show_stats(
	struct seq_file		*m,
	struct xfs_mount	*mp)
{
	struct xfs_zone_info	*zi = mp->m_zone_info;
	struct xfs_open_zone	*oz;

	seq_puts(m, "\n");

	seq_printf(m, "\tuser free RT blocks: %lld\n",
		xfs_sum_freecounter(mp, XC_FREE_RTEXTENTS));
	seq_printf(m, "\treserved free RT blocks: %lld\n",
		mp->m_resblks[XC_FREE_RTEXTENTS].avail);
	seq_printf(m, "\tuser available RT blocks: %lld\n",
		xfs_sum_freecounter(mp, XC_FREE_RTAVAILABLE));
	seq_printf(m, "\treserved available RT blocks: %lld\n",
		mp->m_resblks[XC_FREE_RTAVAILABLE].avail);
	seq_printf(m, "\tRT reservations required: %d\n",
		!list_empty_careful(&zi->zi_reclaim_reservations));
	seq_printf(m, "\tRT GC required: %d\n",
		xfs_zoned_need_gc(mp));

	spin_lock(&zi->zi_zone_list_lock);
	seq_printf(m, "\tfree zones: %d\n", atomic_read(&zi->zi_nr_free_zones));
	seq_puts(m, "\topen zones:\n");
	list_for_each_entry(oz, &zi->zi_open_zones, oz_entry)
		xfs_show_open_zone(m, oz);
	if (zi->zi_open_gc_zone) {
		seq_puts(m, "\topen gc zone:\n");
		xfs_show_open_zone(m, zi->zi_open_gc_zone);
	}
	seq_puts(m, "\tused blocks distribution (fully written zones):\n");
	xfs_show_full_zone_used_distribution(m, mp);
	spin_unlock(&zi->zi_zone_list_lock);
}
