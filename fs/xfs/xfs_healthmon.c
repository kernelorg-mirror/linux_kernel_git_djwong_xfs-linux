// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024-2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_trace.h"
#include "xfs_ag.h"
#include "xfs_btree.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_quota_defs.h"
#include "xfs_rtgroup.h"
#include "xfs_healthmon.h"

#include <linux/anon_inodes.h>
#include <linux/eventpoll.h>
#include <linux/poll.h>

/*
 * Live Health Monitoring
 * ======================
 *
 * Autonomous self-healing of XFS filesystems requires a means for the kernel
 * to send filesystem health events to a monitoring daemon in userspace.  To
 * accomplish this, we establish a thread_with_file kthread object to handle
 * translating internal events about filesystem health into a format that can
 * be parsed easily by userspace.  Then we hook various parts of the filesystem
 * to supply those internal events to the kthread.  Userspace reads events
 * from the file descriptor returned by the ioctl.
 *
 * The healthmon abstraction has a weak reference to the host filesystem mount
 * so that the queueing and processing of the events do not pin the mount and
 * cannot slow down the main filesystem.  The healthmon object can exist past
 * the end of the filesystem mount.
 */

/* Grab a reference to the healthmon object for a given mount, if any. */
struct xfs_healthmon *
xfs_healthmon_get(
	struct xfs_mount		*mp)
{
	struct xfs_healthmon		*hm;

	rcu_read_lock();
	hm = mp->m_healthmon;
	if (hm && !atomic_inc_not_zero(&hm->ref))
		hm = NULL;
	rcu_read_unlock();

	return hm;
}

/* Release the reference to a healthmon object. */
void
xfs_healthmon_put(
	struct xfs_healthmon		*hm)
{
	if (atomic_dec_and_test(&hm->ref))
		wake_up_var(&hm->ref);
}


/*
 * Convey queued event data to userspace.  First copy any remaining bytes in
 * the outbuf, then format the oldest event into the outbuf and copy that too.
 */
STATIC ssize_t
xfs_healthmon_read_iter(
	struct kiocb		*iocb,
	struct iov_iter		*to)
{
	return -EIO;
}

/* Detach a xfs mount from a specific healthmon instance. */
STATIC void
__xfs_healthmon_detach(
	struct xfs_mount	*mp,
	struct xfs_healthmon	*hm)
{
	struct xfs_healthmon	*old = xchg(&mp->m_healthmon, NULL);

	ASSERT(hm == old);

	if (old) {
		xfs_healthmon_put(old);
		old->mp = NULL;
	}
}
/*
 * Detach a xfs mount from its healthmon instance.  Only call this if you hold
 * s_umount.
 */
void
xfs_healthmon_detach(
	struct xfs_mount	*mp)
{
	__xfs_healthmon_detach(mp, mp->m_healthmon);
}

/*
 * While closing the healthmon fd, detach the xfs_mount from this healthmon
 * object.  Only call this from iterate_supers_type.
 */
STATIC void
xfs_healthmon_detach_sb(
	struct super_block	*sb,
	void			*arg)
{
	struct xfs_healthmon	*hm = arg;

	/*
	 * iter_supers_type passed us a non-dying @sb with s_umount held in
	 * shared mode, which means that @sb isn't in deactivate_locked_super
	 * and cannot be freed.  We know that this function cannot be racing
	 * with unmount of the monitored filesystem.
	 *
	 * It is therefore safe to dereference hm->mp to compare against @sb,
	 * and to detach the healthmon from the xfs_mount.
	 */
	if (hm->mp && hm->mp->m_super == sb)
		__xfs_healthmon_detach(hm->mp, hm);
}

/* Free the health monitoring information. */
STATIC int
xfs_healthmon_release(
	struct inode		*inode,
	struct file		*file)
{
	struct xfs_healthmon	*hm = file->private_data;

	/* Stabilize the superblock and detach the healthmon */
	iterate_supers_type(hm->fstyp, xfs_healthmon_detach_sb, hm);

	/*
	 * Drop the file's ref to the healthmon object, then wait for the
	 * refcount to drop to zero.  After this point there are no other
	 * users of the object and we can free it.
	 */
	atomic_dec(&hm->ref);
	wait_var_event(&hm->ref, !atomic_read(&hm->ref));
	kfree_rcu_mightsleep(hm);

	return 0;
}

/* Attach a health monitor to an xfs_mount.  Only one allowed at a time. */
STATIC int
xfs_healthmon_attach(
	struct xfs_mount	*mp,
	struct xfs_healthmon	*hm)
{
	struct xfs_healthmon	*old = NULL;

	if (!try_cmpxchg(&mp->m_healthmon, &old, hm))
		return -EEXIST;

	atomic_inc(&hm->ref);
	return 0;
}

/* Validate ioctl parameters. */
static inline bool
xfs_healthmon_validate(
	const struct xfs_health_monitor	*hmo)
{
	if (hmo->flags)
		return false;
	if (hmo->format)
		return false;
	if (memchr_inv(&hmo->pad1, 0, sizeof(hmo->pad1)))
		return false;
	if (memchr_inv(&hmo->pad2, 0, sizeof(hmo->pad2)))
		return false;
	return true;
}

/* Emit some data about the health monitoring fd. */
#ifdef CONFIG_PROC_FS
static void
xfs_healthmon_show_fdinfo(
	struct seq_file		*m,
	struct file		*file)
{
	struct xfs_healthmon	*hm = file->private_data;

	seq_printf(m, "state:\t%s\ndev:\t%d:%d\n",
			hm->mp ? "alive" : "dead",
			MAJOR(hm->dev), MINOR(hm->dev));
}
#endif

static const struct file_operations xfs_healthmon_fops = {
	.owner		= THIS_MODULE,
#ifdef CONFIG_PROC_FS
	.show_fdinfo	= xfs_healthmon_show_fdinfo,
#endif
	.read_iter	= xfs_healthmon_read_iter,
	.release	= xfs_healthmon_release,
};

/*
 * Create a health monitoring file.  Returns an index to the fd table or a
 * negative errno.
 */
long
xfs_ioc_health_monitor(
	struct file			*file,
	struct xfs_health_monitor __user *arg)
{
	struct xfs_health_monitor	hmo;
	struct xfs_healthmon		*hm;
	struct xfs_inode		*ip = XFS_I(file_inode(file));
	struct xfs_mount		*mp = ip->i_mount;
	int				fd;
	int				ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (ip->i_ino != mp->m_sb.sb_rootino)
		return -EPERM;
	if (current_user_ns() != &init_user_ns)
		return -EPERM;

	if (copy_from_user(&hmo, arg, sizeof(hmo)))
		return -EFAULT;

	if (!xfs_healthmon_validate(&hmo))
		return -EINVAL;

	hm = kzalloc(sizeof(*hm), GFP_KERNEL);
	if (!hm)
		return -ENOMEM;
	hm->mp = mp;
	hm->dev = mp->m_super->s_dev;
	hm->fstyp = mp->m_super->s_type;
	atomic_set(&hm->ref, 1);

	/*
	 * Try to attach this health monitor to the xfs_mount.  The monitor is
	 * considered live and will receive events if this succeeds.
	 */
	ret = xfs_healthmon_attach(mp, hm);
	if (ret)
		goto out_hm;

	/*
	 * Create the anonymous file.  If it succeeds, the file owns hm and
	 * can go away at any time, so we must not access it again.  This must
	 * go last because we can't undo a fd table installation.
	 */
	fd = anon_inode_getfd("xfs_healthmon", &xfs_healthmon_fops, hm,
			O_CLOEXEC | O_RDONLY);
	if (fd < 0) {
		ret = fd;
		goto out_mp;
	}

	return fd;

out_mp:
	__xfs_healthmon_detach(mp, hm);
out_hm:
	ASSERT(atomic_read(&hm->ref) == 1);
	kfree_rcu_mightsleep(hm);
	return ret;
}
