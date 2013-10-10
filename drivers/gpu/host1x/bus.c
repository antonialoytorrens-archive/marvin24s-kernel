/*
 * Copyright (C) 2012 Avionic Design GmbH
 * Copyright (C) 2012-2013, NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/host1x.h>
#include <linux/of.h>
#include <linux/slab.h>

#include "dev.h"

struct host1x_subdev {
	struct host1x_client *client;
	struct device_node *np;
	struct list_head list;
};

static int host1x_subdev_add(struct host1x_device *device,
			     struct device_node *np)
{
	struct host1x_subdev *subdev;

	subdev = kzalloc(sizeof(*subdev), GFP_KERNEL);
	if (!subdev)
		return -ENOMEM;

	INIT_LIST_HEAD(&subdev->list);
	subdev->np = of_node_get(np);

	list_add_tail(&subdev->list, &device->subdevs);

	return 0;
}

static int host1x_device_parse_dt(struct host1x_device *device)
{
	struct device_node *np;
	int err;

	for_each_child_of_node(device->dev.parent->of_node, np) {
		if (of_match_node(device->driver->subdevs, np) &&
		    of_device_is_available(np)) {
			err = host1x_subdev_add(device, np);
			if (err < 0)
				return err;
		}
	}

	return 0;
}

static void host1x_subdev_register(struct host1x_device *device,
				   struct host1x_subdev *subdev,
				   struct host1x_client *client)
{
	int err;

	mutex_lock(&device->subdevs_lock);
	mutex_lock(&device->clients_lock);
	list_del_init(&subdev->list);
	list_add_tail(&client->list, &device->clients);
	client->parent = &device->dev;
	subdev->client = client;
	mutex_unlock(&device->clients_lock);
	mutex_unlock(&device->subdevs_lock);

	if (list_empty(&device->subdevs)) {
		err = device->driver->probe(device);
		if (err < 0)
			dev_err(&device->dev, "probe failed: %d\n", err);
	}
}

static void host1x_subdev_unregister(struct host1x_device *device,
				     struct host1x_subdev *subdev)
{
	mutex_lock(&device->subdevs_lock);
	list_del_init(&subdev->list);
	mutex_unlock(&device->subdevs_lock);

	of_node_put(subdev->np);
	kfree(subdev);
}

int host1x_device_init(struct host1x_device *device)
{
	struct host1x_client *client;
	int err;

	mutex_lock(&device->clients_lock);

	list_for_each_entry(client, &device->clients, list) {
		if (client->ops && client->ops->init) {
			err = client->ops->init(client);
			if (err < 0) {
				dev_err(&device->dev,
					"failed to initialize %s: %d\n",
					dev_name(client->dev), err);
				mutex_unlock(&device->clients_lock);
				return err;
			}
		}
	}

	mutex_unlock(&device->clients_lock);

	return 0;
}

int host1x_device_exit(struct host1x_device *device)
{
	struct host1x_client *client;
	int err;

	mutex_lock(&device->clients_lock);

	list_for_each_entry_reverse(client, &device->clients, list) {
		if (client->ops && client->ops->exit) {
			err = client->ops->exit(client);
			if (err < 0) {
				dev_err(&device->dev,
					"failed to cleanup %s: %d\n",
					dev_name(client->dev), err);
				mutex_unlock(&device->clients_lock);
				return err;
			}
		}
	}

	mutex_unlock(&device->clients_lock);

	return 0;
}

int host1x_register_client(struct host1x *host1x, struct host1x_client *client)
{
	struct host1x_device *device;
	struct host1x_subdev *subdev;

	list_for_each_entry(device, &host1x->devices, list) {
		list_for_each_entry(subdev, &device->subdevs, list) {
			if (subdev->np == client->dev->of_node) {
				host1x_subdev_register(device, subdev, client);
				break;
			}
		}
	}

	return 0;
}

int host1x_unregister_client(struct host1x *host1x,
			     struct host1x_client *client)
{
	struct host1x_device *device;
	struct host1x_subdev *subdev;

	list_for_each_entry(device, &host1x->devices, list)
		list_for_each_entry(subdev, &device->subdevs, list)
			if (subdev->client == client)
				host1x_subdev_unregister(device, subdev);

	return 0;
}

static struct bus_type host1x_bus_type = {
	.name = "host1x",
};

int host1x_bus_init(void)
{
	return bus_register(&host1x_bus_type);
}

void host1x_bus_exit(void)
{
	bus_unregister(&host1x_bus_type);
}

static void host1x_device_release(struct device *dev)
{
	struct host1x_device *device = to_host1x_device(dev);

	kfree(device);
}

static int host1x_device_add(struct host1x *host1x,
			     struct host1x_driver *driver)
{
	struct host1x_device *device;
	int err;

	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device)
		return -ENOMEM;

	mutex_init(&device->subdevs_lock);
	INIT_LIST_HEAD(&device->subdevs);
	mutex_init(&device->clients_lock);
	INIT_LIST_HEAD(&device->clients);
	INIT_LIST_HEAD(&device->list);
	device->driver = driver;

	device->dev.coherent_dma_mask = host1x->dev->coherent_dma_mask;
	device->dev.dma_mask = &device->dev.coherent_dma_mask;
	device->dev.release = host1x_device_release;
	dev_set_name(&device->dev, driver->name);
	device->dev.bus = &host1x_bus_type;
	device->dev.parent = host1x->dev;

	err = device_register(&device->dev);
	if (err < 0)
		return err;

	err = host1x_device_parse_dt(device);
	if (err < 0) {
		put_device(&device->dev);
		return err;
	}

	list_add_tail(&device->list, &host1x->devices);

	return 0;
}

static void host1x_attach_driver(struct host1x *host1x,
				 struct host1x_driver *driver)
{
	struct host1x_device *device;
	int err;

	list_for_each_entry(device, &host1x->devices, list)
		if (device->driver == driver)
			return;

	err = host1x_device_add(host1x, driver);
	if (err < 0)
		dev_err(host1x->dev, "failed to allocate device: %d\n", err);
}

static DEFINE_MUTEX(drivers_lock);
static LIST_HEAD(drivers);

static DEFINE_MUTEX(devices_lock);
static LIST_HEAD(devices);

int host1x_register(struct host1x *host1x)
{
	struct host1x_driver *driver;

	mutex_lock(&devices_lock);
	list_add_tail(&host1x->list, &devices);
	mutex_unlock(&devices_lock);

	list_for_each_entry(driver, &drivers, list)
		host1x_attach_driver(host1x, driver);

	return 0;
}

int host1x_unregister(struct host1x *host1x)
{
	mutex_lock(&devices_lock);
	list_del_init(&host1x->list);
	mutex_unlock(&devices_lock);

	return 0;
}

int host1x_driver_register(struct host1x_driver *driver)
{
	struct host1x *host1x;

	INIT_LIST_HEAD(&driver->list);

	mutex_lock(&drivers_lock);
	list_add_tail(&driver->list, &drivers);
	mutex_unlock(&drivers_lock);

	mutex_lock(&devices_lock);

	list_for_each_entry(host1x, &devices, list)
		host1x_attach_driver(host1x, driver);

	mutex_unlock(&devices_lock);

	return 0;
}
EXPORT_SYMBOL(host1x_driver_register);

void host1x_driver_unregister(struct host1x_driver *driver)
{
	mutex_lock(&drivers_lock);
	list_del_init(&driver->list);
	mutex_unlock(&drivers_lock);
}
EXPORT_SYMBOL(host1x_driver_unregister);
