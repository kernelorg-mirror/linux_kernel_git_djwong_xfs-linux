// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/fs_context.h>
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
	(*envlen)++;
	return ret;
}

#define ADVANCE_ENV(envp, buf, buflen, written) \
	do { \
		ssize_t __written = (written); \
\
		WARN_ON((buflen) < (__written) + 1); \
		*(envp) = (buf); \
		(envp)++; \
		(buf) += (__written) + 1; \
		(buflen) -= (__written) + 1; \
	} while (0)

static char **format_uevent_strings(struct super_block *sb, const char *source)
{
	unsigned int envlen = 0;
	size_t buflen = fs_uevent_bufsize(sb, source, &envlen);
	char *buf;
	char **env, **envp;
	ssize_t written;

	buf = kzalloc(buflen, GFP_KERNEL);
	if (!buf)
		return NULL;
	env = kcalloc(envlen, sizeof(char *), GFP_KERNEL);
	if (!env) {
		kfree(buf);
		return NULL;
	}

	envp = env;
	written = snprintf(buf, buflen, "TYPE=filesystem");
	if (written >= buflen)
		goto bad;
	ADVANCE_ENV(envp, buf, buflen, written);

	written = snprintf(buf, buflen, "SID=%s", sb->s_id);
	if (written >= buflen)
		goto bad;
	ADVANCE_ENV(envp, buf, buflen, written);

	if (source) {
		written = snprintf(buf, buflen, "SOURCE=%s", source);
		if (written >= buflen)
			goto bad;
		ADVANCE_ENV(envp, buf, buflen, written);
	}

	if (sb->s_uuid_len == sizeof(sb->s_uuid)) {
		written = snprintf(buf, buflen, "UUID=%pU", &sb->s_uuid);
		if (written >= buflen)
			goto bad;
		ADVANCE_ENV(envp, buf, buflen, written);
	}

	return env;
bad:
	kfree(env);
	kfree(buf);
	return NULL;
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
	char **env;

	env = format_uevent_strings(sb, NULL);
	if (env) {
		kobject_uevent_env(kobject, kaction, env);
		free_uevent_strings(env);
	}
}
EXPORT_SYMBOL_GPL(fsevent_send);
