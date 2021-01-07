// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/dma-buf/sync_file.c
 *
 * Copyright (C) 2012 Google, Inc.
 */

#include <linux/dma-fence-unwrap.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/anon_inodes.h>
#include <linux/sync_file.h>
#include <uapi/linux/sync_file.h>

static const struct file_operations sync_file_fops;

static struct sync_file *sync_file_alloc(void)
{
	struct sync_file *sync_file;

	sync_file = kzalloc(sizeof(*sync_file), GFP_KERNEL);
	if (!sync_file)
		return NULL;

	sync_file->file = anon_inode_getfile("sync_file", &sync_file_fops,
					     sync_file, 0);
	if (IS_ERR(sync_file->file))
		goto err;

	init_waitqueue_head(&sync_file->wq);

	INIT_LIST_HEAD(&sync_file->cb.node);

	return sync_file;

err:
	kfree(sync_file);
	return NULL;
}

static void fence_check_cb_func(struct dma_fence *f, struct dma_fence_cb *cb)
{
	struct sync_file *sync_file;

	sync_file = container_of(cb, struct sync_file, cb);

	wake_up_all(&sync_file->wq);
}

/**
 * sync_file_create() - creates a sync file
 * @fence:	fence to add to the sync_fence
 *
 * Creates a sync_file containg @fence. This function acquires and additional
 * reference of @fence for the newly-created &sync_file, if it succeeds. The
 * sync_file can be released with fput(sync_file->file). Returns the
 * sync_file or NULL in case of error.
 */
struct sync_file *sync_file_create(struct dma_fence *fence)
{
	struct sync_file *sync_file;

	sync_file = sync_file_alloc();
	if (!sync_file)
		return NULL;

	sync_file->fence = dma_fence_get(fence);

	return sync_file;
}
EXPORT_SYMBOL(sync_file_create);

static struct sync_file *sync_file_fdget(int fd)
{
	struct file *file = fget(fd);

	if (!file)
		return NULL;

	if (file->f_op != &sync_file_fops)
		goto err;

	return file->private_data;

err:
	fput(file);
	return NULL;
}

/**
 * sync_file_get_fence - get the fence related to the sync_file fd
 * @fd:		sync_file fd to get the fence from
 *
 * Ensures @fd references a valid sync_file and returns a fence that
 * represents all fence in the sync_file. On error NULL is returned.
 */
struct dma_fence *sync_file_get_fence(int fd)
{
	struct sync_file *sync_file;
	struct dma_fence *fence;

	sync_file = sync_file_fdget(fd);
	if (!sync_file)
		return NULL;

	fence = dma_fence_get(sync_file->fence);
	fput(sync_file->file);

	return fence;
}
EXPORT_SYMBOL(sync_file_get_fence);

/**
 * sync_file_merge() - merge two sync_files
 * @a:		sync_file a
 * @b:		sync_file b
 *
 * Creates a new sync_file which contains copies of all the fences in both
 * @a and @b.  @a and @b remain valid, independent sync_file. Returns the
 * new merged sync_file or NULL in case of error.
 */
static struct sync_file *sync_file_merge(struct sync_file *a,
					 struct sync_file *b)
{
	struct sync_file *sync_file;
	struct dma_fence *fence;

	sync_file = sync_file_alloc();
	if (!sync_file)
		return NULL;

	fence = dma_fence_unwrap_merge(a->fence, b->fence);
	if (!fence) {
		fput(sync_file->file);
		return NULL;
	}
	sync_file->fence = fence;
	return sync_file;
}

static int sync_file_release(struct inode *inode, struct file *file)
{
	struct sync_file *sync_file = file->private_data;

	if (test_bit(POLL_ENABLED, &sync_file->flags))
		dma_fence_remove_callback(sync_file->fence, &sync_file->cb);
	dma_fence_put(sync_file->fence);
	kfree(sync_file);

	return 0;
}

static __poll_t sync_file_poll(struct file *file, poll_table *wait)
{
	struct sync_file *sync_file = file->private_data;

	poll_wait(file, &sync_file->wq, wait);

	if (list_empty(&sync_file->cb.node) &&
	    !test_and_set_bit(POLL_ENABLED, &sync_file->flags)) {
		if (dma_fence_add_callback(sync_file->fence, &sync_file->cb,
					   fence_check_cb_func) < 0)
			wake_up_all(&sync_file->wq);
	}

	return dma_fence_is_signaled(sync_file->fence) ? EPOLLIN : 0;
}

static long sync_file_ioctl_merge(struct sync_file *sync_file,
				  unsigned long arg)
{
	int fd = get_unused_fd_flags(O_CLOEXEC);
	int err;
	struct sync_file *fence2, *fence3;
	struct sync_merge_data data;
	size_t len;

	if (fd < 0)
		return fd;

	arg += offsetof(typeof(data), fd2);
	len = sizeof(data) - offsetof(typeof(data), fd2);
	if (copy_from_user(&data.fd2, (void __user *)arg, len)) {
		err = -EFAULT;
		goto err_put_fd;
	}

	if (data.flags || data.pad) {
		err = -EINVAL;
		goto err_put_fd;
	}

	fence2 = sync_file_fdget(data.fd2);
	if (!fence2) {
		err = -ENOENT;
		goto err_put_fd;
	}

	fence3 = sync_file_merge(sync_file, fence2);
	if (!fence3) {
		err = -ENOMEM;
		goto err_put_fence2;
	}

	data.fence = fd;
	if (copy_to_user((void __user *)arg, &data.fd2, len)) {
		err = -EFAULT;
		goto err_put_fence3;
	}

	fd_install(fd, fence3->file);
	fput(fence2->file);
	return 0;

err_put_fence3:
	fput(fence3->file);

err_put_fence2:
	fput(fence2->file);

err_put_fd:
	put_unused_fd(fd);
	return err;
}

static int sync_fill_fence_info(struct dma_fence *fence,
				 struct sync_fence_info *info)
{
	info->status = dma_fence_get_status(fence);
	info->timestamp_ns =
		dma_fence_is_signaled(fence) ?
			ktime_to_ns(dma_fence_timestamp(fence)) :
			ktime_set(0, 0);

	return info->status;
}

static long sync_file_ioctl_fence_info(struct sync_file *sync_file,
				       unsigned long arg)
{
	struct dma_fence_unwrap iter;
	struct sync_file_info info;
	unsigned int num_fences;
	struct dma_fence *fence;
	size_t len, offset;

	arg += offsetof(typeof(info), status);
	len = sizeof(info) - offsetof(typeof(info), status);
	if (copy_from_user(&info.status, (void __user *)arg, len))
		return -EFAULT;

	if (info.flags || info.pad)
		return -EINVAL;

	num_fences = 0;
	dma_fence_unwrap_for_each(fence, &iter, sync_file->fence)
		++num_fences;

	/*
	 * Passing num_fences = 0 means that userspace doesn't want to
	 * retrieve any sync_fence_info. If num_fences = 0 we skip filling
	 * sync_fence_info and return the actual number of fences on
	 * info->num_fences.
	 */
	if (!info.num_fences) {
		info.status = dma_fence_get_status(sync_file->fence);
		goto no_fences;
	} else {
		info.status = 1;
	}

	if (info.num_fences < num_fences)
		return -EINVAL;

	num_fences = 0;
	offset = offsetof(struct sync_fence_info, status);
	dma_fence_unwrap_for_each(fence, &iter, sync_file->fence) {
		struct {
			__s32	status;
			__u32	flags;
			__u64	timestamp_ns;
		} fence_info;
		struct sync_fence_info *finfo = (void *)&fence_info - offset;
		int status = sync_fill_fence_info(fence, finfo);
		u64 dest;

		/* Don't leak kernel memory to userspace via finfo->flags */
		finfo->flags = 0;
		info.status = info.status <= 0 ? info.status : status;
		dest = info.sync_fence_info + num_fences++ * sizeof(*finfo) + offset;
		if (copy_to_user(u64_to_user_ptr(dest), &fence_info,
				 sizeof(fence_info)))
			return -EFAULT;
	}

no_fences:
	info.num_fences = num_fences;
	if (copy_to_user((void __user *)arg, &info.status, len))
		return -EFAULT;
	return 0;
}

static long sync_file_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct sync_file *sync_file = file->private_data;

	switch (cmd) {
	case SYNC_IOC_MERGE:
		return sync_file_ioctl_merge(sync_file, arg);

	case SYNC_IOC_FILE_INFO:
		return sync_file_ioctl_fence_info(sync_file, arg);

	default:
		return -ENOTTY;
	}
}

static const struct file_operations sync_file_fops = {
	.release = sync_file_release,
	.poll = sync_file_poll,
	.unlocked_ioctl = sync_file_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};
