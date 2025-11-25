/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef _LINUX_FSUEVENT_H__
#define _LINUX_FSUEVENT_H__

void fs_send_mount_uevent(struct super_block *sb, struct kobject *kobject,
			  struct fs_context *fc);

enum fs_uevent_type {
	FSU_SHUTDOWN,
	FSU_RECONFIGURE,
	FSU_UNMOUNT,
};

void fs_send_uevent(struct super_block *sb, struct kobject *kobject,
		    enum fs_uevent_type action);

#endif /* _LINUX_FSUEVENT_H__ */
