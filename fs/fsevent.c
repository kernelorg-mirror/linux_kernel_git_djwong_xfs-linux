// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/fs_context.h>
#include <linux/seq_buf.h>
#include <linux/fsevent.h>

static inline size_t fs_uevent_bufsize(const struct super_block *sb,
				       const char *source,
				       unsigned int *envlen)
{
	size_t ret = sizeof("TYPE=filesystem") +
		     sizeof("SID=") + sizeof_field(struct super_block, s_id);
	*envlen += 2;

	if (source) {
		ret += sizeof("SOURCE=") + strlen(source) + 1;
		(*envlen)++;
	}

	if (sb->s_uuid_len == sizeof(sb->s_uuid)) {
		ret += sizeof("UUID=") + UUID_STRING_LEN;
		(*envlen)++;
	}

	/* null array element terminator */
	ret++;
	(*envlen)++;
	return ret;
}

static char **format_uevent_strings(struct super_block *sb, const char *source)
{
	struct seq_buf sbuf;
	unsigned int envlen = 0;
	size_t buflen = fs_uevent_bufsize(sb, source, &envlen);
	char *buf;
	char **env, **envp;

	buf = kzalloc(buflen, GFP_KERNEL);
	if (!buf)
		return NULL;
	env = kcalloc(envlen, sizeof(char *), GFP_KERNEL);
	if (!env) {
		kfree(buf);
		return NULL;
	}

	seq_buf_init(&sbuf, buf, buflen);
	envp = env;

	/*
	 * Add a second null terminator on the end so the next printf can start
	 * printing at the second null terminator.
	 */
	seq_buf_get_buf(&sbuf, envp++);
	seq_buf_printf(&sbuf, "TYPE=filesystem");
	seq_buf_putc(&sbuf, 0);

	seq_buf_get_buf(&sbuf, envp++);
	seq_buf_printf(&sbuf, "SID=%s", sb->s_id);
	seq_buf_putc(&sbuf, 0);

	if (source) {
		seq_buf_get_buf(&sbuf, envp++);
		seq_buf_printf(&sbuf, "SOURCE=%s", source);
		seq_buf_putc(&sbuf, 0);
	}

	if (sb->s_uuid_len == sizeof(sb->s_uuid)) {
		seq_buf_get_buf(&sbuf, envp++);
		seq_buf_printf(&sbuf, "UUID=%pU", &sb->s_uuid);
		seq_buf_putc(&sbuf, 0);
	}

	/* Add null terminator to strings array */
	*envp = NULL;

	if (seq_buf_has_overflowed(&sbuf)) {
		WARN_ON(1);
		kfree(env);
		kfree(buf);
		return NULL;
	}

	return env;
}

static inline void free_uevent_strings(char **env)
{
	kfree(env[0]);
	kfree(env);
}

/*
 * Send a uevent signalling that the mount succeeded so we can use udev rules
 * to start background services.
 */
void fsevent_send_mount(struct super_block *sb, struct kobject *kobject,
			struct fs_context *fc)
{
	char **env = format_uevent_strings(sb, fc->source);

	if (env) {
		kobject_uevent_env(kobject, KOBJ_ADD, env);
		free_uevent_strings(env);
	}
}
EXPORT_SYMBOL_GPL(fsevent_send_mount);

/* Send a uevent signalling that something happened to a live mount. */
void fsevent_send(struct super_block *sb, struct kobject *kobject,
		  enum kobject_action kaction)
{
	char **env = format_uevent_strings(sb, NULL);

	if (env) {
		kobject_uevent_env(kobject, kaction, env);
		free_uevent_strings(env);
	}
}
EXPORT_SYMBOL_GPL(fsevent_send);
