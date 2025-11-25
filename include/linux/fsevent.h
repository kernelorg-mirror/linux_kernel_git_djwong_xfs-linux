/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef _LINUX_FSEVENT_H__
#define _LINUX_FSEVENT_H__

void fs_send_mount_uevent(struct super_block *sb, struct kobject *kobject,
			  struct fs_context *fc);

enum fs_event_type {
	FSEVENT_SHUTDOWN,
	FSEVENT_RECONFIGURE,
	FSEVENT_UNMOUNT,
};

void fs_send_uevent(struct super_block *sb, struct kobject *kobject,
		    enum fs_event_type action);

#endif /* _LINUX_FSEVENT_H__ */
