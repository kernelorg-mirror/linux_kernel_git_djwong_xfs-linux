// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE passthrough to backing file.
 *
 * Copyright (c) 2023 CTERA Networks.
 */

#include "fuse_i.h"
#include "fuse_trace.h"

#include <linux/file.h>

struct fuse_backing *fuse_backing_get(struct fuse_backing *fb)
{
	if (fb && refcount_inc_not_zero(&fb->count))
		return fb;
	return NULL;
}

static void fuse_backing_free(struct fuse_backing *fb)
{
	pr_debug("%s: fb=0x%p\n", __func__, fb);

	if (fb->file)
		fput(fb->file);
	put_cred(fb->cred);
	kfree_rcu(fb, rcu);
}

void fuse_backing_put(struct fuse_backing *fb)
{
	if (fb && refcount_dec_and_test(&fb->count))
		fuse_backing_free(fb);
}

void fuse_backing_files_init(struct fuse_conn *fc)
{
	idr_init(&fc->backing_files_map);
}

static int fuse_backing_id_alloc(struct fuse_conn *fc, struct fuse_backing *fb)
{
	int id;

	idr_preload(GFP_KERNEL);
	spin_lock(&fc->lock);
	/* FIXME: xarray might be space inefficient */
	id = idr_alloc_cyclic(&fc->backing_files_map, fb, 1, 0, GFP_ATOMIC);
	spin_unlock(&fc->lock);
	idr_preload_end();

	WARN_ON_ONCE(id == 0);
	return id;
}

static struct fuse_backing *fuse_backing_id_remove(struct fuse_conn *fc,
						   int id)
{
	struct fuse_backing *fb;

	spin_lock(&fc->lock);
	fb = idr_remove(&fc->backing_files_map, id);
	spin_unlock(&fc->lock);

	return fb;
}

static int fuse_backing_id_free(int id, void *p, void *data)
{
	struct fuse_conn *fc = data;
	struct fuse_backing *fb = p;

	WARN_ON_ONCE(refcount_read(&fb->count) != 1);

	trace_fuse_backing_close(fc, id, fb);
	fuse_backing_free(fb);
	return 0;
}

void fuse_backing_files_free(struct fuse_conn *fc)
{
	idr_for_each(&fc->backing_files_map, fuse_backing_id_free, fc);
	idr_destroy(&fc->backing_files_map);
}

int fuse_backing_open(struct fuse_conn *fc, struct fuse_backing_map *map)
{
	struct file *file = NULL;
	struct fuse_backing *fb = NULL;
	int res, passthrough_res, iomap_res;

	pr_debug("%s: fd=%d flags=0x%x\n", __func__, map->fd, map->flags);

	res = -EPERM;
	if (!fc->passthrough && !fc->iomap)
		goto out;

	res = -EINVAL;
	if (map->flags || map->padding)
		goto out;

	file = fget_raw(map->fd);
	res = -EBADF;
	if (!file)
		goto out;

	fb = kmalloc(sizeof(struct fuse_backing), GFP_KERNEL);
	res = -ENOMEM;
	if (!fb)
		goto out_file;

	/* fb now owns file */
	fb->file = file;
	file = NULL;
	fb->cred = prepare_creds();
	refcount_set(&fb->count, 1);

	/*
	 * Each _backing_open function should either:
	 *
	 * 1. Take a ref to fb if it wants the file and return 0.
	 * 2. Return 0 without taking a ref if the backing file isn't needed.
	 * 3. Return an errno explaining why it couldn't attach.
	 *
	 * If at least one subsystem bumps the reference count to open it,
	 * we'll install it into the index and return the index.  If nobody
	 * opens the file, the error code will be passed up.  EPERM is the
	 * default.
	 */
	passthrough_res = fuse_passthrough_backing_open(fc, fb);
	iomap_res = fuse_iomap_backing_open(fc, fb);

	if (refcount_read(&fb->count) < 2) {
		if (passthrough_res)
			res = passthrough_res;
		if (!res && iomap_res)
			res = iomap_res;
		if (!res)
			res = -EPERM;
		goto out_fb;
	}

	res = fuse_backing_id_alloc(fc, fb);
	if (res < 0)
		goto out_fb;

	trace_fuse_backing_open(fc, res, fb);

	pr_debug("%s: fb=0x%p, ret=%i\n", __func__, fb, res);
	fuse_backing_put(fb);
	return res;

out_fb:
	fuse_backing_free(fb);
out_file:
	if (file)
		fput(file);
out:
	pr_debug("%s: ret=%i\n", __func__, res);
	return res;
}

int fuse_backing_close(struct fuse_conn *fc, int backing_id)
{
	struct fuse_backing *fb = NULL, *test_fb;
	int err, passthrough_err, iomap_err;

	pr_debug("%s: backing_id=%d\n", __func__, backing_id);

	err = -EPERM;
	if (!fc->passthrough && !fc->iomap)
		goto out;

	err = -EINVAL;
	if (backing_id <= 0)
		goto out;

	err = -ENOENT;
	fb = fuse_backing_lookup(fc, backing_id);
	if (!fb)
		goto out;

	/*
	 * Each _backing_close function should either:
	 *
	 * 1. Release the ref that it took in _backing_open and return 0.
	 * 2. Don't release the ref if the backing file is busy, and return 0.
	 * 2. Return an errno explaining why it couldn't detach.
	 *
	 * If there are no more active references to the backing file, it will
	 * be closed and removed from the index.  If there are still active
	 * references to the backing file other than the one we just took, the
	 * error code will be passed up.  EBUSY is the default.
	 */
	passthrough_err = fuse_passthrough_backing_close(fc, fb);
	iomap_err = fuse_iomap_backing_close(fc, fb);

	if (refcount_read(&fb->count) > 1) {
		if (passthrough_err)
			err = passthrough_err;
		if (!err && iomap_err)
			err = iomap_err;
		if (!err)
			err = -EBUSY;
		goto out_fb;
	}

	trace_fuse_backing_close(fc, backing_id, fb);

	err = -ENOENT;
	test_fb = fuse_backing_id_remove(fc, backing_id);
	if (!test_fb)
		goto out_fb;

	WARN_ON(fb != test_fb);
	pr_debug("%s: fb=0x%p, err=0\n", __func__, fb);
	fuse_backing_put(fb);
	return 0;
out_fb:
	fuse_backing_put(fb);
out:
	pr_debug("%s: fb=0x%p, err=%i\n", __func__, fb, err);

	return err;
}

struct fuse_backing *fuse_backing_lookup(struct fuse_conn *fc, int backing_id)
{
	struct fuse_backing *fb;

	rcu_read_lock();
	fb = idr_find(&fc->backing_files_map, backing_id);
	fb = fuse_backing_get(fb);
	rcu_read_unlock();

	return fb;
}
