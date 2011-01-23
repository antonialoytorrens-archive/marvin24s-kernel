#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/i2c.h>

#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>

#include <linux/mfd/nvtegra-ec.h>

#define DEVICE_NAME "nvec"

int ecdev_open(struct inode *inode, struct file *file)
{

	printk(KERN_ALERT "nvec: open\n");

	return 0;
}

int ecdev_release(struct inode *inode, struct file *file)
{
        printk(KERN_ALERT "nvec: release\n");

	return 0;
}

ssize_t ecdev_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
//	__nvtegra_ec_read();
        printk(KERN_ALERT "nvec: read\n");

	return 0;
}

ssize_t ecdev_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
//	__nvtegra_ec_write();
        printk(KERN_ALERT "nvec: write\n");

	return 0;
}

static struct file_operations ec_fops = {
	.owner = THIS_MODULE,
	.open = ecdev_open,
	.release = ecdev_release,
	.read = ecdev_read,
	.write = ecdev_write,
};

static struct miscdevice ec_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &ec_fops,
};

static int __init nvtegra_ec_init(void)
{
	int err;
	err = misc_register(&ec_miscdev);
	if (err < 0)
		printk(KERN_ALERT "nvec: error registering misc device\n");
	else
		printk(KERN_ALERT "nvec: misc device registered on minor %d\n", ec_miscdev.minor);

	return err;
}
module_init(nvtegra_ec_init);

static void __exit nvtegra_ec_exit(void)
{
	misc_deregister(&ec_miscdev);
}

module_exit(nvtegra_ec_exit);

MODULE_AUTHOR("Marc Dietrich <marvin24@gmx.de>");
MODULE_DESCRIPTION("EC-SMBus Interface");
MODULE_LICENSE("GPL");
