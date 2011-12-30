/*
 * drivers/video/tegra/dc/dev.c
 *
 * Copyright (C) 2011, NVIDIA Corporation
 *
 * Author: Robert Morell <rmorell@nvidia.com>
 * Some code based on fbdev extensions written by:
 *	Erik Gilling <konkers@android.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <video/tegra_dc_ext.h>

#include <mach/dc.h>
#include <mach/nvmap.h>
#include <mach/tegra_dc_ext.h>

/* XXX ew */
#include "../dc_priv.h"
/* XXX ew 2 */
#include "../../host/dev.h"
/* XXX ew 3 */
#include "../../nvmap/nvmap.h"
#include "tegra_dc_ext_priv.h"

static int tegra_dc_ext_devno;
static struct class *tegra_dc_ext_class;

struct tegra_dc_ext_flip_win {
	struct tegra_dc_ext_flip_windowattr	attr;
	struct nvmap_handle_ref			*handle;
	/* ugh. is this really necessary */
	dma_addr_t				phys_addr;
};

struct tegra_dc_ext_flip_data {
	struct tegra_dc_ext		*ext;
	struct work_struct		work;
	struct tegra_dc_ext_flip_win	win[DC_N_WINDOWS];
	u32				syncpt_max;
};

static int tegra_dc_ext_set_nvmap_fd(struct tegra_dc_ext_user *user,
				     int fd)
{
	struct nvmap_client *nvmap = NULL;

	if (fd >= 0) {
		nvmap = nvmap_client_get_file(fd);
		if (IS_ERR(nvmap))
			return PTR_ERR(nvmap);
	}

	if (user->nvmap)
		nvmap_client_put(user->nvmap);

	user->nvmap = nvmap;

	return 0;
}

static int tegra_dc_ext_get_window(struct tegra_dc_ext_user *user,
				   unsigned int n)
{
	struct tegra_dc_ext *ext = user->ext;
	struct tegra_dc_ext_win *win;
	int ret = 0;

	if (n >= DC_N_WINDOWS)
		return -EINVAL;

	win = &ext->win[n];

	mutex_lock(&win->lock);

	if (!win->user)
		win->user = user;
	else if (win->user != user)
		ret = -EBUSY;

	mutex_unlock(&win->lock);

	return ret;
}

static int tegra_dc_ext_put_window(struct tegra_dc_ext_user *user,
				   unsigned int n)
{
	struct tegra_dc_ext *ext = user->ext;
	struct tegra_dc_ext_win *win;
	int ret = 0;

	if (n >= DC_N_WINDOWS)
		return -EINVAL;

	win = &ext->win[n];

	mutex_lock(&win->lock);

	if (win->user == user)
		win->user = 0;
	else
		ret = -EACCES;

	mutex_unlock(&win->lock);

	return ret;
}

void tegra_dc_ext_suspend(struct tegra_dc_ext *ext)
{
	flush_workqueue(ext->flip_wq);
}

static int tegra_dc_ext_set_windowattr(struct tegra_dc_ext *ext,
			       struct tegra_dc_win *win,
			       const struct tegra_dc_ext_flip_win *flip_win)
{
	struct tegra_dc_ext_win *ext_win = &ext->win[win->idx];

	if (flip_win->handle == NULL) {
		win->flags = 0;
		ext_win->cur_handle = NULL;
		return 0;
	}

	win->flags = TEGRA_WIN_FLAG_ENABLED;
	if (flip_win->attr.blend == TEGRA_DC_EXT_BLEND_PREMULT)
		win->flags |= TEGRA_WIN_FLAG_BLEND_PREMULT;
	else if (flip_win->attr.blend == TEGRA_DC_EXT_BLEND_COVERAGE)
		win->flags |= TEGRA_WIN_FLAG_BLEND_COVERAGE;
	win->fmt = flip_win->attr.pixformat;
	win->x = flip_win->attr.x;
	win->y = flip_win->attr.y;
	win->w = flip_win->attr.w;
	win->h = flip_win->attr.h;
	/* XXX verify that this doesn't go outside display's active region */
	win->out_x = flip_win->attr.out_x;
	win->out_y = flip_win->attr.out_y;
	win->out_w = flip_win->attr.out_w;
	win->out_h = flip_win->attr.out_h;
	win->z = flip_win->attr.z;
	ext_win->cur_handle = flip_win->handle;

	/* XXX verify that this won't read outside of the surface */
	win->phys_addr = flip_win->phys_addr + flip_win->attr.offset;
	win->offset_u = flip_win->attr.offset_u + flip_win->attr.offset;
	win->offset_v = flip_win->attr.offset_v + flip_win->attr.offset;
	win->stride = flip_win->attr.stride;
	win->stride_uv = flip_win->attr.stride_uv;

	if ((s32)flip_win->attr.pre_syncpt_id >= 0) {
		nvhost_syncpt_wait_timeout(&ext->dc->ndev->host->syncpt,
					   flip_win->attr.pre_syncpt_id,
					   flip_win->attr.pre_syncpt_val,
					   msecs_to_jiffies(500));
	}


	return 0;
}

static void tegra_dc_ext_flip_worker(struct work_struct *work)
{
	struct tegra_dc_ext_flip_data *data =
		container_of(work, struct tegra_dc_ext_flip_data, work);
	struct tegra_dc_ext *ext = data->ext;
	struct tegra_dc_win *wins[DC_N_WINDOWS];
	struct nvmap_handle_ref *unpin_handles[DC_N_WINDOWS];
	int i, nr_unpin = 0, nr_win = 0;

	for (i = 0; i < DC_N_WINDOWS; i++) {
		struct tegra_dc_ext_flip_win *flip_win = &data->win[i];
		int index = flip_win->attr.index;
		struct tegra_dc_win *win;
		struct tegra_dc_ext_win *ext_win;

		if (index < 0)
			continue;

		win = tegra_dc_get_window(ext->dc, index);
		ext_win = &ext->win[index];

		if ((win->flags & TEGRA_WIN_FLAG_ENABLED) &&
		    ext_win->cur_handle)
			unpin_handles[nr_unpin++] = ext_win->cur_handle;

		tegra_dc_ext_set_windowattr(ext, win, &data->win[i]);

		wins[nr_win++] = win;
	}

	tegra_dc_update_windows(wins, nr_win);
	/* TODO: implement swapinterval here */
	tegra_dc_sync_windows(wins, nr_win);

	tegra_dc_incr_syncpt_min(ext->dc, data->syncpt_max);

	/* unpin and deref previous front buffers */
	for (i = 0; i < nr_unpin; i++) {
		nvmap_unpin(ext->nvmap, unpin_handles[i]);
		nvmap_free(ext->nvmap, unpin_handles[i]);
	}

	kfree(data);
}

static int tegra_dc_ext_pin_window(struct tegra_dc_ext_user *user,
				   struct tegra_dc_ext_flip_win *flip_win)
{
	struct tegra_dc_ext *ext = user->ext;
	struct nvmap_handle_ref *win_dup;
	struct nvmap_handle *win_handle;
	u32 id = flip_win->attr.buff_id;

	if (!id) {
		flip_win->handle = NULL;
		flip_win->phys_addr = -1;

		return 0;
	}

	/*
	 * Take a reference to the buffer using the user's nvmap context, to
	 * make sure they have permissions to access it.
	 */
	win_handle = nvmap_get_handle_id(user->nvmap, id);
	if (!win_handle)
		return -EACCES;

	/*
	 * Duplicate the buffer's handle into the dc_ext driver's nvmap
	 * context, to ensure that the handle won't be freed as long as it is
	 * in use by display.
	 */
	win_dup = nvmap_duplicate_handle_id(ext->nvmap, id);

	/* Release the reference we took in the user's context above */
	nvmap_handle_put(win_handle);

	if (IS_ERR(win_dup))
		return PTR_ERR(win_dup);

	flip_win->handle = win_dup;

	flip_win->phys_addr = nvmap_pin(ext->nvmap, win_dup);
	/* XXX this isn't correct for non-pointers... */
	if (IS_ERR((void *)flip_win->phys_addr)) {
		nvmap_free(ext->nvmap, win_dup);
		return PTR_ERR((void *)flip_win->phys_addr);
	}

	return 0;
}

static int lock_windows_for_flip(struct tegra_dc_ext_user *user,
				 struct tegra_dc_ext_flip *args)
{
	struct tegra_dc_ext *ext = user->ext;
	int i;

	for (i = 0; i < DC_N_WINDOWS; i++) {
		int index = args->win[i].index;
		struct tegra_dc_ext_win *win;

		if (index < 0)
			continue;

		win = &ext->win[index];

		mutex_lock(&win->lock);

		if (win->user != user)
			goto fail_unlock;
	}

	return 0;

fail_unlock:
	do {
		int index = args->win[i].index;

		if (index < 0)
			continue;

		mutex_unlock(&ext->win[index].lock);
	} while (i--);

	return -EACCES;
}

static void unlock_windows_for_flip(struct tegra_dc_ext_user *user,
				    struct tegra_dc_ext_flip *args)
{
	struct tegra_dc_ext *ext = user->ext;
	int i;

	for (i = 0; i < DC_N_WINDOWS; i++) {
		int index = args->win[i].index;

		if (index < 0)
			continue;

		mutex_unlock(&ext->win[index].lock);
	}
}

static int sanitize_flip_args(struct tegra_dc_ext_user *user,
			      struct tegra_dc_ext_flip *args)
{
	int i, used_windows = 0;

	for (i = 0; i < DC_N_WINDOWS; i++) {
		int index = args->win[i].index;

		if (index < 0)
			continue;

		if (index >= DC_N_WINDOWS)
			return -EINVAL;

		if (used_windows & BIT(index))
			return -EINVAL;

		used_windows |= BIT(index);
	}

	if (!used_windows)
		return -EINVAL;

	return 0;
}

static int tegra_dc_ext_flip(struct tegra_dc_ext_user *user,
			     struct tegra_dc_ext_flip *args)
{
	struct tegra_dc_ext *ext = user->ext;
	struct tegra_dc_ext_flip_data *data;
	u32 syncpt_max;
	int i, ret = 0;

	if (!user->nvmap)
		return -EFAULT;

	ret = sanitize_flip_args(user, args);
	if (ret)
		return ret;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	INIT_WORK(&data->work, tegra_dc_ext_flip_worker);
	data->ext = ext;

	for (i = 0; i < DC_N_WINDOWS; i++) {
		struct tegra_dc_ext_flip_win *flip_win = &data->win[i];
		int index = args->win[i].index;

		memcpy(&flip_win->attr, &args->win[i], sizeof(flip_win->attr));

		if (index < 0)
			continue;

		ret = tegra_dc_ext_pin_window(user, flip_win);
		if (ret)
			goto fail_pin;
	}

	ret = lock_windows_for_flip(user, args);
	if (ret)
		goto fail_pin;

	syncpt_max = tegra_dc_incr_syncpt_max(ext->dc);
	data->syncpt_max = syncpt_max;

	args->post_syncpt_val = syncpt_max;
	args->post_syncpt_id = tegra_dc_get_syncpt_id(ext->dc);

	queue_work(ext->flip_wq, &data->work);

	unlock_windows_for_flip(user, args);

	return 0;

fail_pin:
	while (i--) {
		if (!data->win[i].handle)
			continue;

		nvmap_unpin(ext->nvmap, data->win[i].handle);
		nvmap_free(ext->nvmap, data->win[i].handle);
	}
	kfree(data);

	return ret;
}

static long tegra_dc_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	void __user *user_arg = (void __user *)arg;
	struct tegra_dc_ext_user *user = filp->private_data;

	switch (cmd) {
	case TEGRA_DC_EXT_SET_NVMAP_FD:
		return tegra_dc_ext_set_nvmap_fd(user, arg);

	case TEGRA_DC_EXT_GET_WINDOW:
		return tegra_dc_ext_get_window(user, arg);
	case TEGRA_DC_EXT_PUT_WINDOW:
		return tegra_dc_ext_put_window(user, arg);

	case TEGRA_DC_EXT_FLIP:
	{
		struct tegra_dc_ext_flip args;
		int ret;

		if (copy_from_user(&args, user_arg, sizeof(args)))
			return -EFAULT;

		ret = tegra_dc_ext_flip(user, &args);

		if (copy_to_user(user_arg, &args, sizeof(args)))
			return -EFAULT;

		return ret;
	}

	default:
		return -EINVAL;
	}
}

static int tegra_dc_open(struct inode *inode, struct file *filp)
{
	struct tegra_dc_ext_user *user;
	struct tegra_dc_ext *ext;

	user = kzalloc(sizeof(*user), GFP_KERNEL);
	if (!user)
		return -ENOMEM;

	ext = container_of(inode->i_cdev, struct tegra_dc_ext, cdev);
	user->ext = ext;

	filp->private_data = user;

	return 0;
}

static int tegra_dc_release(struct inode *inode, struct file *filp)
{
	struct tegra_dc_ext_user *user = filp->private_data;
	struct tegra_dc_ext *ext = user->ext;
	unsigned int i;

	for (i = 0; i < DC_N_WINDOWS; i++) {
		if (ext->win[i].user == user)
			tegra_dc_ext_put_window(user, i);
	}

	if (user->nvmap)
		nvmap_client_put(user->nvmap);

	kfree(user);

	return 0;
}

static int tegra_dc_ext_setup_windows(struct tegra_dc_ext *ext)
{
	int i;

	for (i = 0; i < ext->dc->n_windows; i++) {
		struct tegra_dc_ext_win *win = &ext->win[i];

		win->ext = ext;
		win->idx = i;

		mutex_init(&win->lock);
	}

	return 0;
}

static const struct file_operations tegra_dc_devops = {
	.owner =		THIS_MODULE,
	.open =			tegra_dc_open,
	.release =		tegra_dc_release,
	.unlocked_ioctl =	tegra_dc_ioctl,
};

struct tegra_dc_ext *tegra_dc_ext_register(struct nvhost_device *ndev,
					   struct tegra_dc *dc)
{
	int ret;
	struct tegra_dc_ext *ext;

	ext = kzalloc(sizeof(*ext), GFP_KERNEL);
	if (!ext)
		return ERR_PTR(-ENOMEM);

	BUG_ON(!tegra_dc_ext_devno);
	cdev_init(&ext->cdev, &tegra_dc_devops);
	ext->cdev.owner = THIS_MODULE;
	ret = cdev_add(&ext->cdev, tegra_dc_ext_devno, 1);
	if (ret) {
		dev_err(&ndev->dev, "Failed to create character device\n");
		goto cleanup_alloc;
	}

	ext->dev = device_create(tegra_dc_ext_class,
				 &ndev->dev,
				 tegra_dc_ext_devno,
				 NULL,
				 "tegra_dc_%d",
				 ndev->id);

	if (IS_ERR(ext->dev)) {
		ret = PTR_ERR(ext->dev);
		goto cleanup_cdev;
	}

	ext->dc = dc;

	ext->nvmap = nvmap_create_client(nvmap_dev, "tegra_dc_ext");
	if (!ext->nvmap) {
		ret = -ENOMEM;
		goto cleanup_device;
	}

	ext->flip_wq = create_singlethread_workqueue(dev_name(&ndev->dev));
	if (!ext->flip_wq) {
		ret = -ENOMEM;
		goto cleanup_nvmap;
	}

	ret = tegra_dc_ext_setup_windows(ext);
	if (ret)
		goto cleanup_wq;

	tegra_dc_ext_devno++;

	return ext;

cleanup_wq:
	destroy_workqueue(ext->flip_wq);

cleanup_nvmap:
	nvmap_client_put(ext->nvmap);

cleanup_device:
	device_del(ext->dev);

cleanup_cdev:
	cdev_del(&ext->cdev);

cleanup_alloc:
	kfree(ext);

	return ERR_PTR(ret);
}

void tegra_dc_ext_unregister(struct tegra_dc_ext *ext)
{

	flush_workqueue(ext->flip_wq);
	destroy_workqueue(ext->flip_wq);

	nvmap_client_put(ext->nvmap);
	device_del(ext->dev);
	cdev_del(&ext->cdev);

	kfree(ext);
}

int __init tegra_dc_ext_module_init(void)
{
	int ret;

	tegra_dc_ext_class = class_create(THIS_MODULE, "tegra_dc_ext");
	if (!tegra_dc_ext_class) {
		printk(KERN_ERR "tegra_dc_ext: failed to create class\n");
		return -ENOMEM;
	}

	ret = alloc_chrdev_region(&tegra_dc_ext_devno,
				  0, TEGRA_MAX_DC,
				  "tegra_dc_ext");
	if (ret)
		class_destroy(tegra_dc_ext_class);

	return ret;
}

void __exit tegra_dc_ext_module_exit(void)
{
	unregister_chrdev_region(tegra_dc_ext_devno, TEGRA_MAX_DC);
	class_destroy(tegra_dc_ext_class);
}
