/* qcusbnet.c - gobi network device
 * Copyright (c) 2011, Code Aurora Forum. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "structs.h"
#include "qmidevice.h"
#include "qmi.h"
#include "qcusbnet.h"

#include <linux/ctype.h>

#define DRIVER_VERSION "1.0.110+google"
#define DRIVER_AUTHOR "Qualcomm Innovation Center"
#define DRIVER_DESC "gobi"

static LIST_HEAD(qcusbnet_list);
static DEFINE_MUTEX(qcusbnet_lock);

int qcusbnet_debug;
static struct class *devclass;

static void free_dev(struct kref *ref)
{
	struct qcusbnet *dev = container_of(ref, struct qcusbnet, refcount);
	list_del(&dev->node);
	kfree(dev);
}

void qcusbnet_put(struct qcusbnet *dev)
{
	mutex_lock(&qcusbnet_lock);
	kref_put(&dev->refcount, free_dev);
	mutex_unlock(&qcusbnet_lock);
}

struct qcusbnet *qcusbnet_get(struct qcusbnet *key)
{
	/* Given a putative qcusbnet struct, return either the struct itself
	 * (with a ref taken) if the struct is still visible, or NULL if it's
	 * not. This prevents object-visibility races where someone is looking
	 * up an object as the last ref gets dropped; dropping the last ref and
	 * removing the object from the list are atomic with respect to getting
	 * a new ref. */
	struct qcusbnet *entry;
	mutex_lock(&qcusbnet_lock);
	list_for_each_entry(entry, &qcusbnet_list, node) {
		if (entry == key) {
			kref_get(&entry->refcount);
			mutex_unlock(&qcusbnet_lock);
			return entry;
		}
	}
	mutex_unlock(&qcusbnet_lock);
	return NULL;
}

int qc_suspend(struct usb_interface *iface, pm_message_t event)
{
	struct usbnet *usbnet;
	struct qcusbnet *dev;

	if (!iface)
		return -ENOMEM;

	usbnet = usb_get_intfdata(iface);

	if (!usbnet || !usbnet->net) {
		DBG("failed to get netdevice\n");
		return -ENXIO;
	}

	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		DBG("failed to get QMIDevice\n");
		return -ENXIO;
	}

	if (!(event.event & PM_EVENT_AUTO)) {
		DBG("device suspended to power level %d\n",
		    event.event);
		qc_setdown(dev, DOWN_DRIVER_SUSPENDED);
	} else {
		DBG("device autosuspend\n");
	}

	if (event.event & PM_EVENT_SUSPEND) {
		qc_stopread(dev);
		usbnet->udev->reset_resume = 0;
		iface->dev.power.power_state.event = event.event;
	} else {
		usbnet->udev->reset_resume = 1;
	}

	return usbnet_suspend(iface, event);
}

static int qc_resume(struct usb_interface *iface)
{
	struct usbnet *usbnet;
	struct qcusbnet *dev;
	int ret;
	int oldstate;

	if (iface == 0)
		return -ENOMEM;

	usbnet = usb_get_intfdata(iface);

	if (!usbnet || !usbnet->net) {
		DBG("failed to get netdevice\n");
		return -ENXIO;
	}

	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		DBG("failed to get QMIDevice\n");
		return -ENXIO;
	}

	oldstate = iface->dev.power.power_state.event;
	iface->dev.power.power_state.event = PM_EVENT_ON;
	DBG("resuming from power mode %d\n", oldstate);

	if (oldstate & PM_EVENT_SUSPEND) {
		qc_cleardown(dev, DOWN_DRIVER_SUSPENDED);

		ret = usbnet_resume(iface);
		if (ret) {
			DBG("usbnet_resume error %d\n", ret);
			return ret;
		}

		ret = qc_startread(dev);
		if (ret) {
			DBG("qc_startread error %d\n", ret);
			return ret;
		}

		complete(&dev->worker.work);
	} else {
		DBG("nothing to resume\n");
		return 0;
	}

	return ret;
}

static int qcnet_bind(struct usbnet *usbnet, struct usb_interface *iface)
{
	int numends;
	int i;
	struct usb_host_endpoint *endpoint = NULL;
	struct usb_host_endpoint *in = NULL;
	struct usb_host_endpoint *out = NULL;

	if (iface->num_altsetting != 1) {
		DBG("invalid num_altsetting %u\n", iface->num_altsetting);
		return -EINVAL;
	}

	if (iface->cur_altsetting->desc.bInterfaceNumber != 0
	    && iface->cur_altsetting->desc.bInterfaceNumber != 5) {
		DBG("invalid interface %d\n",
			  iface->cur_altsetting->desc.bInterfaceNumber);
		return -EINVAL;
	}

	numends = iface->cur_altsetting->desc.bNumEndpoints;
	for (i = 0; i < numends; i++) {
		endpoint = iface->cur_altsetting->endpoint + i;
		if (!endpoint) {
			DBG("invalid endpoint %u\n", i);
			return -EINVAL;
		}

		if (usb_endpoint_dir_in(&endpoint->desc)
		&&  !usb_endpoint_xfer_int(&endpoint->desc)) {
			in = endpoint;
		} else if (!usb_endpoint_dir_out(&endpoint->desc)) {
			out = endpoint;
		}
	}

	if (!in || !out) {
		DBG("invalid endpoints\n");
		return -EINVAL;
	}

	if (usb_set_interface(usbnet->udev,
			      iface->cur_altsetting->desc.bInterfaceNumber, 0))	{
		DBG("unable to set interface\n");
		return -EINVAL;
	}

	usbnet->in = usb_rcvbulkpipe(usbnet->udev, in->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
	usbnet->out = usb_sndbulkpipe(usbnet->udev, out->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);

	DBG("in %x, out %x\n",
	    in->desc.bEndpointAddress,
	    out->desc.bEndpointAddress);

	return 0;
}

static void qcnet_unbind(struct usbnet *usbnet, struct usb_interface *iface)
{
	struct qcusbnet *dev = (struct qcusbnet *)usbnet->data[0];

	netif_carrier_off(usbnet->net);
	qc_deregister(dev);

	kfree(usbnet->net->netdev_ops);
	usbnet->net->netdev_ops = NULL;
	/* drop the list's ref */
	qcusbnet_put(dev);
}

static void qcnet_urbhook(struct urb *urb)
{
	unsigned long flags;
	struct worker *worker = urb->context;
	if (!worker) {
		DBG("bad context\n");
		return;
	}

	if (urb->status) {
		DBG("urb finished with error %d\n", urb->status);
	}

	spin_lock_irqsave(&worker->active_lock, flags);
	worker->active = ERR_PTR(-EAGAIN);
	spin_unlock_irqrestore(&worker->active_lock, flags);
	/* XXX-fix race against qcnet_stop()? */
	complete(&worker->work);
	usb_free_urb(urb);
}

static void qcnet_txtimeout(struct net_device *netdev)
{
	struct list_head *node, *tmp;
	struct qcusbnet *dev;
	struct worker *worker;
	struct urbreq *req;
	unsigned long activeflags, listflags;
	struct usbnet *usbnet = netdev_priv(netdev);

	if (!usbnet || !usbnet->net) {
		DBG("failed to get usbnet device\n");
		return;
	}

	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		DBG("failed to get QMIDevice\n");
		return;
	}
	worker = &dev->worker;

	DBG("\n");

	spin_lock_irqsave(&worker->active_lock, activeflags);
	if (worker->active)
		usb_kill_urb(worker->active);
	spin_unlock_irqrestore(&worker->active_lock, activeflags);

	spin_lock_irqsave(&worker->urbs_lock, listflags);
	list_for_each_safe(node, tmp, &worker->urbs) {
		req = list_entry(node, struct urbreq, node);
		usb_free_urb(req->urb);
		list_del(&req->node);
		kfree(req);
	}
	spin_unlock_irqrestore(&worker->urbs_lock, listflags);

	complete(&worker->work);
}

static int qcnet_worker(void *arg)
{
	struct list_head *node, *tmp;
	unsigned long activeflags, listflags;
	struct urbreq *req;
	int status;
	struct usb_device *usbdev;
	struct worker *worker = arg;
	if (!worker) {
		DBG("passed null pointer\n");
		return -EINVAL;
	}

	usbdev = interface_to_usbdev(worker->iface);

	DBG("traffic thread started\n");

	while (!worker->exit && !kthread_should_stop()) {
		wait_for_completion_interruptible(&worker->work);

		if (worker->exit || kthread_should_stop()) {
			spin_lock_irqsave(&worker->active_lock, activeflags);
			if (worker->active) {
				usb_kill_urb(worker->active);
			}
			spin_unlock_irqrestore(&worker->active_lock, activeflags);

			spin_lock_irqsave(&worker->urbs_lock, listflags);
			list_for_each_safe(node, tmp, &worker->urbs) {
				req = list_entry(node, struct urbreq, node);
				usb_free_urb(req->urb);
				list_del(&req->node);
				kfree(req);
			}
			spin_unlock_irqrestore(&worker->urbs_lock, listflags);

			break;
		}

		spin_lock_irqsave(&worker->active_lock, activeflags);
		if (IS_ERR(worker->active) && PTR_ERR(worker->active) == -EAGAIN) {
			worker->active = NULL;
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			usb_autopm_put_interface(worker->iface);
			spin_lock_irqsave(&worker->active_lock, activeflags);
		}

		if (worker->active) {
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			continue;
		}

		spin_lock_irqsave(&worker->urbs_lock, listflags);
		if (list_empty(&worker->urbs)) {
			spin_unlock_irqrestore(&worker->urbs_lock, listflags);
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			continue;
		}

		req = list_first_entry(&worker->urbs, struct urbreq, node);
		list_del(&req->node);
		spin_unlock_irqrestore(&worker->urbs_lock, listflags);

		worker->active = req->urb;
		spin_unlock_irqrestore(&worker->active_lock, activeflags);

		status = usb_autopm_get_interface(worker->iface);
		if (status < 0) {
			DBG("unable to autoresume interface: %d\n", status);
			if (status == -EPERM) {
				qc_suspend(worker->iface, PMSG_SUSPEND);
			}

			spin_lock_irqsave(&worker->urbs_lock, listflags);
			list_add(&req->node, &worker->urbs);
			spin_unlock_irqrestore(&worker->urbs_lock, listflags);

			spin_lock_irqsave(&worker->active_lock, activeflags);
			worker->active = NULL;
			spin_unlock_irqrestore(&worker->active_lock, activeflags);

			continue;
		}

		status = usb_submit_urb(worker->active, GFP_KERNEL);
		if (status < 0) {
			DBG("Failed to submit URB: %d.  Packet dropped\n", status);
			spin_lock_irqsave(&worker->active_lock, activeflags);
			usb_free_urb(worker->active);
			worker->active = NULL;
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			usb_autopm_put_interface(worker->iface);
			complete(&worker->work);
		}

		kfree(req);
	}

	DBG("traffic thread exiting\n");
	worker->thread = NULL;
	return 0;
}

static int qcnet_startxmit(struct sk_buff *skb, struct net_device *netdev)
{
	unsigned long listflags;
	struct qcusbnet *dev;
	struct worker *worker;
	struct urbreq *req;
	void *data;
	struct usbnet *usbnet = netdev_priv(netdev);

	DBG("\n");

	if (!usbnet || !usbnet->net) {
		DBG("failed to get usbnet device\n");
		return NETDEV_TX_BUSY;
	}

	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		DBG("failed to get QMIDevice\n");
		return NETDEV_TX_BUSY;
	}
	worker = &dev->worker;

	if (qc_isdown(dev, DOWN_DRIVER_SUSPENDED)) {
		DBG("device is suspended\n");
		dump_stack();
		return NETDEV_TX_BUSY;
	}

	req = kmalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		DBG("unable to allocate URBList memory\n");
		return NETDEV_TX_BUSY;
	}

	req->urb = usb_alloc_urb(0, GFP_ATOMIC);

	if (!req->urb) {
		kfree(req);
		DBG("unable to allocate URB\n");
		return NETDEV_TX_BUSY;
	}

	data = kmalloc(skb->len, GFP_ATOMIC);
	if (!data) {
		usb_free_urb(req->urb);
		kfree(req);
		DBG("unable to allocate URB data\n");
		return NETDEV_TX_BUSY;
	}
	memcpy(data, skb->data, skb->len);

	usb_fill_bulk_urb(req->urb, dev->usbnet->udev, dev->usbnet->out,
			  data, skb->len, qcnet_urbhook, worker);

	spin_lock_irqsave(&worker->urbs_lock, listflags);
	list_add_tail(&req->node, &worker->urbs);
	spin_unlock_irqrestore(&worker->urbs_lock, listflags);

	complete(&worker->work);

	netdev->trans_start = jiffies;
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

static int qcnet_open(struct net_device *netdev)
{
	int status = 0;
	struct qcusbnet *dev;
	struct usbnet *usbnet = netdev_priv(netdev);

	if (!usbnet) {
		DBG("failed to get usbnet device\n");
		return -ENXIO;
	}

	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		DBG("failed to get QMIDevice\n");
		return -ENXIO;
	}

	DBG("\n");

	dev->worker.iface = dev->iface;
	INIT_LIST_HEAD(&dev->worker.urbs);
	dev->worker.active = NULL;
	spin_lock_init(&dev->worker.urbs_lock);
	spin_lock_init(&dev->worker.active_lock);
	init_completion(&dev->worker.work);

	dev->worker.exit = 0;
	dev->worker.thread = kthread_run(qcnet_worker, &dev->worker, "qcnet_worker");
	if (IS_ERR(dev->worker.thread)) {
		DBG("AutoPM thread creation error\n");
		return PTR_ERR(dev->worker.thread);
	}

	qc_cleardown(dev, DOWN_NET_IFACE_STOPPED);
	if (dev->open) {
		status = dev->open(netdev);
		if (status == 0) {
			usb_autopm_put_interface(dev->iface);
		}
	} else {
		DBG("no USBNetOpen defined\n");
	}

	return status;
}

int qcnet_stop(struct net_device *netdev)
{
	struct qcusbnet *dev;
	struct usbnet *usbnet = netdev_priv(netdev);

	if (!usbnet || !usbnet->net) {
		DBG("failed to get netdevice\n");
		return -ENXIO;
	}

	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		DBG("failed to get QMIDevice\n");
		return -ENXIO;
	}

	qc_setdown(dev, DOWN_NET_IFACE_STOPPED);
	dev->worker.exit = 1;
	complete(&dev->worker.work);
	kthread_stop(dev->worker.thread);
	DBG("thread stopped\n");

	if (dev->stop != NULL)
		return dev->stop(netdev);
	return 0;
}

static const struct driver_info qc_netinfo = {
	.description   = "QCUSBNet Ethernet Device",
	.flags         = FLAG_ETHER,
	.bind          = qcnet_bind,
	.unbind        = qcnet_unbind,
	.data          = 0,
};

#define MKVIDPID(v, p)					\
{							\
	USB_DEVICE(v, p),				\
	.driver_info = (unsigned long)&qc_netinfo,	\
}

static const struct usb_device_id qc_vidpids[] = {
	MKVIDPID(0x05c6, 0x9215),	/* Acer Gobi 2000 */
	MKVIDPID(0x05c6, 0x9265),	/* Asus Gobi 2000 */
	MKVIDPID(0x16d8, 0x8002),	/* CMOTech Gobi 2000 */
	MKVIDPID(0x413c, 0x8186),	/* Dell Gobi 2000 */
	MKVIDPID(0x1410, 0xa010),	/* Entourage Gobi 2000 */
	MKVIDPID(0x1410, 0xa011),	/* Entourage Gobi 2000 */
	MKVIDPID(0x1410, 0xa012),	/* Entourage Gobi 2000 */
	MKVIDPID(0x1410, 0xa013),	/* Entourage Gobi 2000 */
	MKVIDPID(0x03f0, 0x251d),	/* HP Gobi 2000 */
	MKVIDPID(0x05c6, 0x9205),	/* Lenovo Gobi 2000 */
	MKVIDPID(0x05c6, 0x920b),	/* Generic Gobi 2000 */
	MKVIDPID(0x04da, 0x250f),	/* Panasonic Gobi 2000 */
	MKVIDPID(0x05c6, 0x9245),	/* Samsung Gobi 2000 */
	MKVIDPID(0x1199, 0x9001),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9002),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9003),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9004),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9005),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9006),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9007),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9008),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9009),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x900a),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x05c6, 0x9225),	/* Sony Gobi 2000 */
	MKVIDPID(0x05c6, 0x9235),	/* Top Global Gobi 2000 */
	MKVIDPID(0x05c6, 0x9275),	/* iRex Technologies Gobi 2000 */

	MKVIDPID(0x05c6, 0x920d),	/* Qualcomm Gobi 3000 */
	MKVIDPID(0x1410, 0xa021),	/* Novatel Gobi 3000 */
	{ }
};

MODULE_DEVICE_TABLE(usb, qc_vidpids);

static u8 nibble(unsigned char c)
{
	if (likely(isdigit(c)))
		return c - '0';
	c = toupper(c);
	if (likely(isxdigit(c)))
		return 10 + c - 'A';
	return 0;
}

int qcnet_probe(struct usb_interface *iface, const struct usb_device_id *vidpids)
{
	int status;
	struct usbnet *usbnet;
	struct qcusbnet *dev;
	struct net_device_ops *netdevops;
	int i;
	u8 *addr;

	status = usbnet_probe(iface, vidpids);
	if (status < 0) {
		DBG("usbnet_probe failed %d\n", status);
		return status;
	}

	usbnet = usb_get_intfdata(iface);

	if (!usbnet || !usbnet->net) {
		DBG("failed to get netdevice\n");
		return -ENXIO;
	}

	dev = kmalloc(sizeof(struct qcusbnet), GFP_KERNEL);
	if (!dev) {
		DBG("failed to allocate device buffers\n");
		return -ENOMEM;
	}

	usbnet->data[0] = (unsigned long)dev;

	dev->usbnet = usbnet;

	netdevops = kmalloc(sizeof(struct net_device_ops), GFP_KERNEL);
	if (!netdevops) {
		DBG("failed to allocate net device ops\n");
		return -ENOMEM;
	}
	memcpy(netdevops, usbnet->net->netdev_ops, sizeof(struct net_device_ops));

	dev->open = netdevops->ndo_open;
	netdevops->ndo_open = qcnet_open;
	dev->stop = netdevops->ndo_stop;
	netdevops->ndo_stop = qcnet_stop;
	netdevops->ndo_start_xmit = qcnet_startxmit;
	netdevops->ndo_tx_timeout = qcnet_txtimeout;

	usbnet->net->netdev_ops = netdevops;

	memset(&(dev->usbnet->net->stats), 0, sizeof(struct net_device_stats));

	dev->iface = iface;
	memset(&(dev->meid), '0', 14);

	dev->valid = false;
	memset(&dev->qmi, 0, sizeof(dev->qmi));

	dev->qmi.devclass = devclass;

	kref_init(&dev->refcount);
	INIT_LIST_HEAD(&dev->node);
	INIT_LIST_HEAD(&dev->qmi.clients);
	init_completion(&dev->worker.work);
	spin_lock_init(&dev->qmi.clients_lock);

	dev->down = 0;
	qc_setdown(dev, DOWN_NO_NDIS_CONNECTION);
	qc_setdown(dev, DOWN_NET_IFACE_STOPPED);

	status = qc_register(dev);
	if (status) {
		qc_deregister(dev);
	} else {
		mutex_lock(&qcusbnet_lock);
		/* Give our initial ref to the list */
		list_add(&dev->node, &qcusbnet_list);
		mutex_unlock(&qcusbnet_lock);
	}
	/* After calling qc_register, MEID is valid */
	addr = &usbnet->net->dev_addr[0];
	for (i = 0; i < 6; i++)
		addr[i] = (nibble(dev->meid[i*2+2]) << 4)+
			nibble(dev->meid[i*2+3]);
	addr[0] &= 0xfe;		/* clear multicast bit */
	addr[0] |= 0x02;		/* set local assignment bit (IEEE802) */

	return status;
}
EXPORT_SYMBOL_GPL(qcnet_probe);

static struct usb_driver qcusbnet = {
	.name       = "gobi",
	.id_table   = qc_vidpids,
	.probe      = qcnet_probe,
	.disconnect = usbnet_disconnect,
	.suspend    = qc_suspend,
	.resume     = qc_resume,
	.supports_autosuspend = true,
};

static int __init modinit(void)
{
	devclass = class_create(THIS_MODULE, "QCQMI");
	if (IS_ERR(devclass)) {
		DBG("error at class_create %ld\n", PTR_ERR(devclass));
		return -ENOMEM;
	}
	printk(KERN_INFO "%s: %s\n", DRIVER_DESC, DRIVER_VERSION);
	return usb_register(&qcusbnet);
}
module_init(modinit);

static void __exit modexit(void)
{
	usb_deregister(&qcusbnet);
	class_destroy(devclass);
}
module_exit(modexit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("Dual BSD/GPL");

module_param(qcusbnet_debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(qcusbnet_debug, "Debugging enabled or not");
