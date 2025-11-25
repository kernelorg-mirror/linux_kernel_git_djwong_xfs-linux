/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef _LINUX_FSEVENT_H__
#define _LINUX_FSEVENT_H__

void fsevent_send_mount(struct super_block *sb, struct kobject *kobject,
			struct fs_context *fc);

void fsevent_send(struct super_block *sb, struct kobject *kobject,
		  enum kobject_action kaction);

static inline void fsevent_send_unmount(struct super_block *sb,
					struct kobject *kobject)
{
	fsevent_send(sb, kobject, KOBJ_REMOVE);
}

static inline void fsevent_send_remount(struct super_block *sb,
					struct kobject *kobject)
{
	fsevent_send(sb, kobject, KOBJ_CHANGE);
}

static inline void fsevent_send_shutdown(struct super_block *sb,
					 struct kobject *kobject)
{
	fsevent_send(sb, kobject, KOBJ_OFFLINE);
}

#endif /* _LINUX_FSEVENT_H__ */
