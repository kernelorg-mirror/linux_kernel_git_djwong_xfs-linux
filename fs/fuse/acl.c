/*
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2016 Canonical Ltd. <seth.forshee@canonical.com>
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#include "fuse_i.h"
#include "fuse_iomap.h"

#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/fs_struct.h>

/*
 * If this fuse server behaves like a local filesystem, we can implement the
 * kernel's optimizations for ACLs for local filesystems instead of passing
 * the ACL requests straight through to another server.
 */
static inline bool fuse_inode_has_local_acls(const struct inode *inode)
{
	const struct fuse_conn *fc = get_fuse_conn(inode);

	return fc->posix_acl && fuse_inode_is_exclusive(inode);
}

static struct posix_acl *__fuse_get_acl(struct fuse_conn *fc,
					struct inode *inode, int type, bool rcu)
{
	int size;
	const char *name;
	void *value = NULL;
	struct posix_acl *acl;

	if (rcu)
		return ERR_PTR(-ECHILD);

	if (fuse_is_bad(inode))
		return ERR_PTR(-EIO);

	if (fc->no_getxattr)
		return NULL;

	if (type == ACL_TYPE_ACCESS)
		name = XATTR_NAME_POSIX_ACL_ACCESS;
	else if (type == ACL_TYPE_DEFAULT)
		name = XATTR_NAME_POSIX_ACL_DEFAULT;
	else
		return ERR_PTR(-EOPNOTSUPP);

	value = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!value)
		return ERR_PTR(-ENOMEM);
	size = fuse_getxattr(inode, name, value, PAGE_SIZE);
	if (size > 0)
		acl = posix_acl_from_xattr(fc->user_ns, value, size);
	else if ((size == 0) || (size == -ENODATA) ||
		 (size == -EOPNOTSUPP && fc->no_getxattr))
		acl = NULL;
	else if (size == -ERANGE)
		acl = ERR_PTR(-E2BIG);
	else
		acl = ERR_PTR(size);

	kfree(value);
	return acl;
}

static inline bool fuse_no_acl(const struct fuse_conn *fc,
			       const struct inode *inode)
{
	/*
	 * Refuse interacting with POSIX ACLs for daemons that
	 * don't support FUSE_POSIX_ACL and are not mounted on
	 * the host to retain backwards compatibility.
	 */
	return !fc->posix_acl && (i_user_ns(inode) != &init_user_ns);
}

struct posix_acl *fuse_get_acl(struct mnt_idmap *idmap,
			       struct dentry *dentry, int type)
{
	struct inode *inode = d_inode(dentry);
	struct fuse_conn *fc = get_fuse_conn(inode);

	if (fuse_no_acl(fc, inode))
		return ERR_PTR(-EOPNOTSUPP);

	return __fuse_get_acl(fc, inode, type, false);
}

struct posix_acl *fuse_get_inode_acl(struct inode *inode, int type, bool rcu)
{
	struct fuse_conn *fc = get_fuse_conn(inode);

	/*
	 * FUSE daemons before FUSE_POSIX_ACL was introduced could get and set
	 * POSIX ACLs without them being used for permission checking by the
	 * vfs. Retain that behavior for backwards compatibility as there are
	 * filesystems that do all permission checking for acls in the daemon
	 * and not in the kernel.
	 */
	if (!fc->posix_acl)
		return NULL;
	return __fuse_get_acl(fc,  inode, type, rcu);
}

int fuse_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		 struct posix_acl *acl, int type)
{
	struct inode *inode = d_inode(dentry);
	struct fuse_conn *fc = get_fuse_conn(inode);
	const char *name;
	umode_t mode = inode->i_mode;
	const bool local_acls = fuse_inode_has_local_acls(inode);
	const bool is_iomap = fuse_inode_has_iomap(inode);
	int ret;

	if (fuse_is_bad(inode))
		return -EIO;

	if (fc->no_setxattr || fuse_no_acl(fc, inode))
		return -EOPNOTSUPP;

	if (type == ACL_TYPE_ACCESS)
		name = XATTR_NAME_POSIX_ACL_ACCESS;
	else if (type == ACL_TYPE_DEFAULT)
		name = XATTR_NAME_POSIX_ACL_DEFAULT;
	else
		return -EINVAL;

	/*
	 * If the ACL can be represented entirely with changes to the mode
	 * bits, then most filesystems will update the mode bits and delete
	 * the ACL xattr.
	 */
	if (acl && type == ACL_TYPE_ACCESS && local_acls) {
		ret = posix_acl_update_mode(idmap, inode, &mode, &acl);
		if (ret)
			return ret;
	}

	if (acl) {
		unsigned int extra_flags = 0;
		/*
		 * For non-local filesystems, fuse userspace is responsible for
		 * updating access permissions in the inode, if needed.
		 * fuse_setxattr invalidates the inode attributes, which will
		 * force them to be refreshed the next time they are used, and
		 * it also updates i_ctime.
		 */
		size_t size;
		void *value;

		value = posix_acl_to_xattr(fc->user_ns, acl, &size, GFP_KERNEL);
		if (!value)
			return -ENOMEM;

		if (size > PAGE_SIZE) {
			kfree(value);
			return -E2BIG;
		}

		/*
		 * Fuse daemons without FUSE_POSIX_ACL never changed the passed
		 * through POSIX ACLs. Such daemons don't expect setgid bits to
		 * be stripped, unless they've explicitly told the kernel to
		 * take care of that.
		 */
		if (fc->posix_acl && !local_acls &&
		    !in_group_or_capable(idmap, inode,
					 i_gid_into_vfsgid(idmap, inode)))
			extra_flags |= FUSE_SETXATTR_ACL_KILL_SGID;

		ret = fuse_setxattr(inode, name, value, size, 0, extra_flags);
		kfree(value);
	} else {
		ret = fuse_removexattr(inode, name);
		/* If the acl didn't exist to start with that's fine. */
		if (ret == -ENODATA)
			ret = 0;
	}

	if (!ret) {
		struct iattr attr = { };

		/*
		 * When we're running in iomap mode, we need to update mode and
		 * ctime ourselves instead of letting the fuse server figure
		 * that out.
		 */
		if (is_iomap) {
			attr.ia_valid |= ATTR_CTIME;
			inode_set_ctime_current(inode);
			attr.ia_ctime = inode_get_ctime(inode);
		}

		/*
		 * If we scheduled a mode update above, push that to userspace
		 * now.  We set the mode after successfully updating the ACL
		 * xattr because the xattr update can fail at ENOSPC and we
		 * don't want to change the mode if the ACL update hasn't been
		 * applied.
		 */
		if (mode != inode->i_mode) {
			attr.ia_valid |= ATTR_MODE;
			attr.ia_mode = mode;
		}

		if (attr.ia_valid) {
			ret = fuse_do_setattr(idmap, dentry, &attr, NULL);
			if (!ret)
				inode->i_mode = mode;
		}
	}

	if (fc->posix_acl) {
		/*
		 * Fuse daemons without FUSE_POSIX_ACL never cached POSIX ACLs
		 * and didn't invalidate attributes. Retain that behavior
		 * except for iomap, where we assume that only the source of
		 * ACL changes is userspace.
		 */
		if (!ret && is_iomap) {
			set_cached_acl(inode, type, acl);
		} else {
			forget_all_cached_acls(inode);
			fuse_invalidate_attr(inode);
		}
	}

	return ret;
}

int fuse_acl_create(struct inode *dir, umode_t *mode,
		    struct posix_acl **default_acl, struct posix_acl **acl)
{
	struct fuse_conn *fc = get_fuse_conn(dir);

	if (fuse_is_bad(dir))
		return -EIO;

	if (IS_POSIXACL(dir) && fuse_inode_has_local_acls(dir))
		return posix_acl_create(dir, mode, default_acl, acl);

	if (!fc->dont_mask)
		*mode &= ~current_umask();

	*default_acl = NULL;
	*acl = NULL;
	return 0;
}

static int fuse_set_acl_xattr(struct inode *inode, const char *name,
			      const struct posix_acl *acl)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	void *value __free(kfree) = NULL;
	size_t size;

	value = posix_acl_to_xattr(fc->user_ns, acl, &size, GFP_KERNEL);
	if (!value)
		return -ENOMEM;

	if (size > PAGE_SIZE)
		return -E2BIG;

	return fuse_setxattr(inode, name, value, size, 0, 0);
}

int fuse_init_acls(struct inode *inode, const struct posix_acl *default_acl,
		   const struct posix_acl *acl)
{
	int ret;

	if (default_acl) {
		ret = fuse_set_acl_xattr(inode, XATTR_NAME_POSIX_ACL_DEFAULT,
					 default_acl);
		if (ret)
			return ret;
	}

	if (acl) {
		ret = fuse_set_acl_xattr(inode, XATTR_NAME_POSIX_ACL_ACCESS,
					 acl);
		if (ret)
			return ret;
	}

	return 0;
}
