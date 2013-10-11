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

	dev_info(&device->dev, "> %s(device=%p, np=%p)\n", __func__, device, np);

	subdev = kzalloc(sizeof(*subdev), GFP_KERNEL);
	if (!subdev)
		return -ENOMEM;

	INIT_LIST_HEAD(&subdev->list);
	subdev->np = of_node_get(np);

	list_add_tail(&subdev->list, &device->subdevs);

	dev_info(&device->dev, "< %s()\n", __func__);
	return 0;
}

static void host1x_subdev_del(struct host1x_subdev *subdev)
{
	list_del(&subdev->list);
	of_node_put(subdev->np);
	kfree(subdev);
}

static int host1x_device_parse_dt(struct host1x_device *device)
{
	struct device_node *np;
	int err;

	dev_info(&device->dev, "> %s(device=%p)\n", __func__, device);

	for_each_child_of_node(device->dev.parent->of_node, np) {
		if (of_match_node(device->driver->subdevs, np) &&
		    of_device_is_available(np)) {
			err = host1x_subdev_add(device, np);
			if (err < 0) {
				dev_info(&device->dev, "< %s() = %d\n", __func__, err);
				return err;
			}
		}
	}

	dev_info(&device->dev, "< %s()\n", __func__);
	return 0;
}

static void host1x_subdev_register(struct host1x_device *device,
				   struct host1x_subdev *subdev,
				   struct host1x_client *client)
{
	int err;

	dev_info(&device->dev, "> %s(device=%p, subdev=%p, client=%p)\n",
		 __func__, device, subdev, client);
	dev_info(&device->dev, "  registering %s...\n", dev_name(client->dev));

	mutex_lock(&device->subdevs_lock);
	mutex_lock(&device->clients_lock);
	list_move_tail(&client->list, &device->clients);
	list_move_tail(&subdev->list, &device->active);
	client->parent = &device->dev;
	subdev->client = client;
	mutex_unlock(&device->clients_lock);
	mutex_unlock(&device->subdevs_lock);

	if (list_empty(&device->subdevs)) {
		err = device->driver->probe(device);
		if (err < 0)
			dev_err(&device->dev, "probe failed: %d\n", err);
	}

	dev_info(&device->dev, "< %s()\n", __func__);
}

static void host1x_subdev_unregister(struct host1x_device *device,
				     struct host1x_subdev *subdev)
{
	struct host1x_client *client = subdev->client;
	int err;

	dev_info(&device->dev, "> %s(device=%p, subdev=%p)\n", __func__,
		 device, subdev);

	if (list_empty(&device->subdevs)) {
		err = device->driver->remove(device);
		if (err < 0)
			dev_err(&device->dev, "remove failed: %d\n", err);
	}

	mutex_lock(&device->clients_lock);
	mutex_lock(&device->subdevs_lock);
	subdev->client = NULL;
	client->parent = NULL;
	list_move_tail(&subdev->list, &device->subdevs);
	list_del_init(&client->list);
	mutex_unlock(&device->subdevs_lock);
	mutex_unlock(&device->clients_lock);

	//of_node_put(subdev->np);
	//kfree(subdev);

	list_for_each_entry(client, &device->clients, list)
		dev_info(&device->dev, "  client: %p (%s)\n", client,
			 dev_name(client->dev));

	dev_info(&device->dev, "< %s()\n", __func__);
}

int host1x_device_init(struct host1x_device *device)
{
	struct host1x_client *client;
	int err;

	dev_info(&device->dev, "> %s(device=%p)\n", __func__, device);

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

	dev_info(&device->dev, "< %s()\n", __func__);
	return 0;
}

static DEFINE_MUTEX(clients_lock);
static LIST_HEAD(clients);

int host1x_device_exit(struct host1x_device *device)
{
	struct host1x_client *client;
	int err;

	dev_info(&device->dev, "> %s(device=%p)\n", __func__, device);

	mutex_lock(&device->clients_lock);

	list_for_each_entry_reverse(client, &device->clients, list) {
		dev_info(&device->dev, "  running exit for %p (%s)...\n", client,
			 dev_name(client->dev));

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

	list_for_each_entry(client, &device->clients, list) {
		dev_info(&device->dev, "  client: %p (%s)\n", client,
			 dev_name(client->dev));
	}

	mutex_unlock(&device->clients_lock);

	dev_info(&device->dev, "< %s()\n", __func__);
	return 0;
}

static int host1x_register_client(struct host1x *host1x,
				  struct host1x_client *client)
{
	struct host1x_device *device;
	struct host1x_subdev *subdev;

	dev_info(host1x->dev, "> %s(host1x=%p, client=%p)\n", __func__, host1x, client);
	dev_info(host1x->dev, "  client: %s\n", dev_name(client->dev));

	list_for_each_entry(device, &host1x->devices, list) {
		dev_info(host1x->dev, "  checking device %s...\n", dev_name(&device->dev));
		dev_info(host1x->dev, "    subdevs: %p, %p\n", device->subdevs.prev, device->subdevs.next);
		list_for_each_entry(subdev, &device->subdevs, list) {
			dev_info(host1x->dev, "    checking subdevice %p\n", subdev);
			dev_info(host1x->dev, "      np: %p\n", subdev->np);
			dev_info(host1x->dev, "        name: %s\n", subdev->np->name);
			if (subdev->np == client->dev->of_node) {
				host1x_subdev_register(device, subdev, client);
				dev_info(host1x->dev, "< %s()\n", __func__);
				return 0;
			}
		}
	}

	dev_info(host1x->dev, "< %s() = -ENODEV\n", __func__);
	return -ENODEV;
}

static int host1x_unregister_client(struct host1x *host1x,
				    struct host1x_client *client)
{
	struct host1x_device *device, *dt;
	struct host1x_subdev *subdev;

	list_for_each_entry_safe(device, dt, &host1x->devices, list) {
		list_for_each_entry(subdev, &device->active, list) {
			if (subdev->client == client) {
				host1x_subdev_unregister(device, subdev);
				return 0;
			}
		}
	}

	return -ENODEV;
}

struct bus_type host1x_bus_type = {
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

	pr_info("> %s(dev=%p)\n", __func__, dev);

	kfree(device);

	pr_info("< %s()\n", __func__);
}

static int host1x_device_add(struct host1x *host1x,
			     struct host1x_driver *driver)
{
	struct host1x_client *client, *tmp;
	struct host1x_subdev *subdev;
	struct host1x_device *device;
	int err;

	dev_info(host1x->dev, "> %s(host1x=%p, driver=%p)\n", __func__, host1x, driver);

	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device)
		return -ENOMEM;

	mutex_init(&device->subdevs_lock);
	INIT_LIST_HEAD(&device->subdevs);
	INIT_LIST_HEAD(&device->active);
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
		device_unregister(&device->dev);
		return err;
	}

	list_add_tail(&device->list, &host1x->devices);

	mutex_lock(&clients_lock);

	list_for_each_entry_safe(client, tmp, &clients, list) {
		dev_info(host1x->dev, "  trying idle client %p...\n", client);
		list_for_each_entry(subdev, &device->subdevs, list) {
			dev_info(host1x->dev, "    checking subdevice %p\n", subdev);
			dev_info(host1x->dev, "      np: %p\n", subdev->np);
			dev_info(host1x->dev, "        name: %s\n", subdev->np->name);
			if (subdev->np == client->dev->of_node) {
				host1x_subdev_register(device, subdev, client);
				break;
			}
		}
	}

	mutex_unlock(&clients_lock);

	dev_info(host1x->dev, "< %s()\n", __func__);
	return 0;
}

static void host1x_device_del(struct host1x *host1x,
			      struct host1x_device *device)
{
	/*
	struct kobject *kobj = &device->dev.kobj;
	struct kref *kref = &kobj->kref;
	*/
	struct host1x_subdev *subdev, *sd;
	struct host1x_client *client, *cl;

	dev_info(host1x->dev, "> %s(host1x=%p, device=%p)\n", __func__, host1x, device);
	/*
	dev_info(host1x->dev, "  kobj: %p\n", kobj);
	dev_info(host1x->dev, "    name: %s\n", kobj->name);
	dev_info(host1x->dev, "  kref: %p\n", kref);
	dev_info(host1x->dev, "    refcount: %d\n", atomic_read(&kref->refcount));
	*/
	dev_info(host1x->dev, "  driver: %p\n", device->driver);

	/* unregister subdevices */
	list_for_each_entry_safe(subdev, sd, &device->active, list) {
		dev_info(&device->dev, "  removing active subdevice: %p (%s)\n",
			 subdev, subdev->np->name);
		client = subdev->client;
		host1x_subdev_unregister(device, subdev);

		mutex_lock(&clients_lock);
		list_add_tail(&client->list, &clients);
		mutex_unlock(&clients_lock);
	}

	/* remove subdevices */
	list_for_each_entry_safe(subdev, sd, &device->subdevs, list) {
		dev_info(host1x->dev, "  removing subdevice %p (%s)...\n",
			 subdev, subdev->np->name);
		host1x_subdev_del(subdev);
	}

	/* move clients to idle list */
	mutex_lock(&clients_lock);
	mutex_lock(&device->clients_lock);

	list_for_each_entry_safe(client, cl, &device->clients, list) {
		dev_info(&device->dev, "  moving %p (%s) to idle clients list...\n",
			 client, dev_name(client->dev));
		list_move_tail(&client->list, &clients);
	}

	if (!list_empty(&clients)) {
		dev_info(&device->dev, "idle clients:\n");

		list_for_each_entry(client, &clients, list)
			dev_info(&device->dev, "  %p: %s\n", client,
				 dev_name(client->dev));
	}

	mutex_unlock(&device->clients_lock);
	mutex_unlock(&clients_lock);

	/*
	dev_info(host1x->dev, "    refcount: %d\n", atomic_read(&kref->refcount));
	*/
	list_del_init(&device->list);
	device_unregister(&device->dev);

	dev_info(host1x->dev, "< %s()\n", __func__);
}

static void host1x_attach_driver(struct host1x *host1x,
				 struct host1x_driver *driver)
{
	struct host1x_device *device;
	int err;

	dev_info(host1x->dev, "> %s(host1x=%p, driver=%p)\n", __func__, host1x, driver);

	list_for_each_entry(device, &host1x->devices, list)
		if (device->driver == driver)
			return;

	err = host1x_device_add(host1x, driver);
	if (err < 0)
		dev_err(host1x->dev, "failed to allocate device: %d\n", err);

	dev_info(host1x->dev, "< %s()\n", __func__);
}

static void host1x_detach_driver(struct host1x *host1x,
				 struct host1x_driver *driver)
{
	struct host1x_device *device, *tmp;

	dev_info(host1x->dev, "> %s(host1x=%p, driver=%p)\n", __func__, host1x, driver);

	list_for_each_entry_safe(device, tmp, &host1x->devices, list)
		if (device->driver == driver)
			host1x_device_del(host1x, device);

	dev_info(host1x->dev, "< %s()\n", __func__);
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
	struct host1x_driver *driver;

	pr_info("> %s(host1x=%p)\n", __func__, host1x);

	list_for_each_entry(driver, &drivers, list)
		host1x_detach_driver(host1x, driver);

	mutex_lock(&devices_lock);
	list_del_init(&host1x->list);
	mutex_unlock(&devices_lock);

	pr_info("< %s()\n", __func__);
	return 0;
}

int host1x_driver_register(struct host1x_driver *driver)
{
	struct host1x *host1x;

	pr_info("> %s(driver=%p)\n", __func__, driver);

	INIT_LIST_HEAD(&driver->list);

	mutex_lock(&drivers_lock);
	list_add_tail(&driver->list, &drivers);
	mutex_unlock(&drivers_lock);

	mutex_lock(&devices_lock);

	list_for_each_entry(host1x, &devices, list)
		host1x_attach_driver(host1x, driver);

	mutex_unlock(&devices_lock);

	pr_info("< %s()\n", __func__);
	return 0;
}
EXPORT_SYMBOL(host1x_driver_register);

void host1x_driver_unregister(struct host1x_driver *driver)
{
	pr_info("> %s(driver=%p)\n", __func__, driver);

	mutex_lock(&drivers_lock);
	list_del_init(&driver->list);
	mutex_unlock(&drivers_lock);

	pr_info("< %s()\n", __func__);
}
EXPORT_SYMBOL(host1x_driver_unregister);

int host1x_client_register(struct host1x_client *client)
{
	struct host1x *host1x;
	int err;

	mutex_lock(&devices_lock);

	list_for_each_entry(host1x, &devices, list) {
		err = host1x_register_client(host1x, client);
		if (!err) {
			mutex_unlock(&devices_lock);
			return 0;
		}
	}

	mutex_unlock(&devices_lock);

	pr_info("adding %p (%s) to idle clients list...\n", client,
		dev_name(client->dev));
	mutex_lock(&clients_lock);
	list_add_tail(&client->list, &clients);
	mutex_unlock(&clients_lock);

	return 0;
}

int host1x_client_unregister(struct host1x_client *client)
{
	struct host1x_client *c;
	struct host1x *host1x;
	int err;

	dev_info(client->dev, "> %s(client=%p)\n", __func__, client);

	mutex_lock(&devices_lock);

	list_for_each_entry(host1x, &devices, list) {
		err = host1x_unregister_client(host1x, client);
		if (!err) {
			dev_info(client->dev, "  removed from %s...\n",
				 dev_name(host1x->dev));
			dev_info(client->dev, "< %s()\n", __func__);
			mutex_unlock(&devices_lock);
			return 0;
		}
	}

	mutex_unlock(&devices_lock);
	mutex_lock(&clients_lock);

	list_for_each_entry(c, &clients, list) {
		if (c == client) {
			list_del_init(&c->list);
			break;
		}
	}

	mutex_unlock(&clients_lock);

	dev_info(client->dev, "< %s()\n", __func__);
	return 0;
}
