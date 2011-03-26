/*
 *  chromeos_acpi.c - ChromeOS specific ACPI support
 *
 *
 * Copyright (C) 2011 The Chromium OS Authors
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * This driver attaches to the ChromeOS ACPI device and the exports the values
 * reported by the ACPI in a sysfs directory
 * (/sys/devices/platform/chromeos_acpi).
 *
 * The first version of the driver provides only static information; the
 * values reported by the driver are the snapshot reported by the ACPI at
 * driver installation time.
 *
 * All values are presented in the string form (numbers as decimal values) and
 * can be accessed as the contents of the appropriate read only files in the
 * sysfs directory tree originating in /sys/devices/platform/chromeos_acpi.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>

MODULE_AUTHOR("Google Inc.");
MODULE_DESCRIPTION("Chrome OS Extras Driver");
MODULE_LICENSE("GPL");

#define MY_LOGPREFIX "chromeos_acpi: "
#define MY_ERR KERN_ERR MY_LOGPREFIX
#define MY_NOTICE KERN_NOTICE MY_LOGPREFIX
#define MY_INFO KERN_INFO MY_LOGPREFIX

/* ACPI method name for MLST; the response for this method is a
 * package of strings listing the methods which should be reflected in
 * sysfs. */
#define MLST_METHOD "MLST"

static const struct acpi_device_id chromeos_device_ids[] = {
	{"GGL0001", 0}, /* Google's own */
	{"", 0},
};

MODULE_DEVICE_TABLE(acpi, chromeos_device_ids);
static int chromeos_device_add(struct acpi_device *device);
static int chromeos_device_remove(struct acpi_device *device, int type);

static struct acpi_driver chromeos_acpi_driver = {
	.name = "ChromeOS Device",
	.class = "ChromeOS",
	.ids = chromeos_device_ids,
	.ops = {
		.add = chromeos_device_add,
		.remove = chromeos_device_remove,
		},
	.owner = THIS_MODULE,
};

/* The default list of methods the chromeos ACPI device is supposed to export,
 * if the MLST method is not present or is poorly formed.  The MLST method
 * itself is included, to aid in debugging. */
static char *default_methods[] = {
	"CHSW", "HWID", "BINF", "GPIO", "CHNV", "FWID", "FRID", MLST_METHOD
};

/*
 * Representation of a single sys fs attribute. In addition to the standard
 * device_attribute structure has a link field, allowing to create a list of
 * these structures (to keep track for de-allocation when removing the driver)
 * and a pointer to the actual attribute value, reported when accessing the
 * appropriate sys fs file
 */
struct acpi_attribute {
	struct device_attribute dev_attr;
	struct acpi_attribute *next_acpi_attr;
	char *value;
};

/*
 * Representation of a sys fs attribute group (a sub directory in the device's
 * sys fs directory). In addition to the standard structure has a link to
 * allow to keep track of the allocated structures.
 */
struct acpi_attribute_group {
	struct attribute_group ag;
	struct acpi_attribute_group *next_acpi_attr_group;
};

/*
 * ChromeOS ACPI device wrapper adds links pointing at lists of allocated
 * attributes and attribute groups.
 */
struct chromeos_acpi_dev {
	struct platform_device *p_dev;
	struct acpi_attribute *attributes;
	struct acpi_attribute_group *groups;
};

static struct chromeos_acpi_dev chromeos_acpi = { };


/* Values set at probe time */
int chromeos_acpi_chnv = -1;
int chromeos_acpi_chsw = -1;

bool chromeos_acpi_available;


/*
 * To show attribute value just access the container structure's `value'
 * field.
 */
static ssize_t show_acpi_attribute(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct acpi_attribute *paa;

	paa = container_of(attr, struct acpi_attribute, dev_attr);
	return snprintf(buf, PAGE_SIZE, paa->value);
}

/*
 * create_sysfs_attribute() create and initialize an ACPI sys fs attribute
 *			    structure.
 * @value: attribute value
 * @name: base attribute name
 * @count: total number of instances of this attribute
 * @instance: instance number of this particular attribute
 *
 * This function allocates and initializes the structure containing all
 * information necessary to add a sys fs attribute. In case the attribute has
 * just a single instance, the attribute file name is equal to the @name
 * parameter . In case the attribute has several instances, the attribute
 * file name is @name.@instance.
 *
 * Returns: a pointer to the allocated and initialized structure, or null if
 * allocation failed.
 *
 * As a side effect, the allocated structure is added to the list in the
 * chromeos_acpi structure. Note that the actual attribute creation is not
 * attempted yet, in case of creation error the structure would not have an
 * actual attribute associated with it, so when de-installing the driver this
 * structure would be used to try to remove an attribute which does not exist.
 * This is considered acceptable, as there is no reason for sys fs attribute
 * creation failure.
 */
static struct acpi_attribute *create_sysfs_attribute(char *value, char *name,
						     int count, int instance)
{
	struct acpi_attribute *paa;
	int total_size, room_left;
	int value_len = strlen(value);

	if (!value_len)
		return NULL;

	value_len++; /* include the terminating zero */

	/*
	 * total allocation size includes (all strings with including
	 * terminating zeros):
	 *
	 * - value string
	 * - attribute structure size
	 * - name string
	 * - suffix string (in case there are multiple instances)
	 * - dot separating the instance suffix
	 */

	total_size = value_len + sizeof(struct acpi_attribute) +
			strlen(name) + 1;

	if (count != 1) {
		if (count >= 1000) {
			printk(MY_ERR "%s: too many (%d) instances of %s\n",
			       __func__, count, name);
			return NULL;
		}
		/* allow up to three digits and the dot */
		total_size += 4;
	}

	paa = kzalloc(total_size, GFP_KERNEL);
	if (!paa) {
		printk(MY_ERR "out of memory in %s!\n", __func__);
		return NULL;
	}

	paa->dev_attr.attr.mode = 0444;  /* read only */
	paa->dev_attr.show = show_acpi_attribute;
	paa->value = (char *)(paa + 1);
	strcpy(paa->value, value);
	paa->dev_attr.attr.name = paa->value + value_len;

	room_left = total_size - value_len -
			offsetof(struct acpi_attribute, value);

	if (count == 1) {
		snprintf((char *)paa->dev_attr.attr.name, room_left, name);
	} else {
		snprintf((char *)paa->dev_attr.attr.name, room_left,
			 "%s.%d", name, instance);
	}

	paa->next_acpi_attr = chromeos_acpi.attributes;
	chromeos_acpi.attributes = paa;

	return paa;
}

/*
 * add_sysfs_attribute() create and initialize an ACPI sys fs attribute
 *			    structure and create the attribute.
 * @value: attribute value
 * @name: base attribute name
 * @count: total number of instances of this attribute
 * @instance: instance number of this particular attribute
 */

static void add_sysfs_attribute(char *value, char *name,
				int count, int instance)
{
	struct acpi_attribute *paa =
	    create_sysfs_attribute(value, name, count, instance);

	if (!paa)
		return;

	if (device_create_file(&chromeos_acpi.p_dev->dev, &paa->dev_attr))
		printk(MY_ERR "failed to create attribute for %s\n", name);
}

/*
 * handle_nested_acpi_package() create sysfs group including attributes
 *				representing a nested ACPI package.
 *
 * @po: package contents as returned by ACPI
 * @pm: name of the group
 * @total: number of instances of this package
 * @instance: instance number of this particular group
 *
 * The created group is called @pm in case there is a single instance, or
 * @pm.@instance otherwise.
 *
 * All group and attribute storage allocations are included in the lists for
 * tracking of allocated memory.
 */
static void handle_nested_acpi_package(union acpi_object *po, char *pm,
				       int total, int instance)
{
	int i, size, count, j;
	struct acpi_attribute_group *aag;

	count = po->package.count;

	size = strlen(pm) + 1 + sizeof(struct acpi_attribute_group) +
	    sizeof(struct attribute *) * (count + 1);

	if (total != 1) {
		if (total >= 1000) {
			printk(MY_ERR "%s: too many (%d) instances of %s\n",
			       __func__, total, pm);
			return;
		}
		/* allow up to three digits and the dot */
		size += 4;
	}

	aag = kzalloc(size, GFP_KERNEL);
	if (!aag) {
		printk(MY_ERR "out of memory in %s!\n", __func__);
		return;
	}

	aag->next_acpi_attr_group = chromeos_acpi.groups;
	chromeos_acpi.groups = aag->next_acpi_attr_group;
	aag->ag.attrs = (struct attribute **)(aag + 1);
	aag->ag.name = (const char *)&aag->ag.attrs[count + 1];

	/* room left in the buffer */
	size = size - (aag->ag.name - (char *)aag);

	if (total != 1)
		snprintf((char *)aag->ag.name, size, "%s.%d", pm, instance);
	else
		snprintf((char *)aag->ag.name, size, "%s", pm);

	j = 0;			/* attribute index */
	for (i = 0; i < count; i++) {
		union acpi_object *element = po->package.elements + i;
		int copy_size = 0;
		char attr_value[40];	/* 40 chars be enough for names */
		struct acpi_attribute *paa;

		switch (element->type) {
		case ACPI_TYPE_INTEGER:
			copy_size = snprintf(attr_value, sizeof(attr_value),
					     "%d", (int)element->integer.value);
			paa = create_sysfs_attribute(attr_value, pm, count, i);
			break;

		case ACPI_TYPE_STRING:
			copy_size = min(element->string.length,
					sizeof(attr_value) - 1);
			memcpy(attr_value, element->string.pointer, copy_size);
			attr_value[copy_size] = '\0';
			paa = create_sysfs_attribute(attr_value, pm, count, i);
			break;

		default:
			printk(MY_ERR "ignoring nested type %d\n",
			       element->type);
			continue;
		}
		aag->ag.attrs[j++] = &paa->dev_attr.attr;
	}

	if (sysfs_create_group(&chromeos_acpi.p_dev->dev.kobj, &aag->ag))
		printk(MY_ERR "failed to create group %s.%d\n", pm, instance);
}

/*
 * handle_single_int() extract a single int value
 *
 * @po: package contents as returned by ACPI
 * @found:	integer pointer to store the value in
 *
 */
static void handle_single_int(union acpi_object *po, int *found)
{
	union acpi_object *element = po->package.elements;

	if (!element) {
		WARN_ON(1);
		return;
	}

	if (element->type == ACPI_TYPE_INTEGER)
		*found = (int) element->integer.value;
	else
		printk(MY_ERR "acpi_object unexpected type %d, expected int\n",
		       element->type);
}


/*
 * handle_acpi_package() create sysfs group including attributes
 *			 representing an ACPI package.
 *
 * @po: package contents as returned by ACPI
 * @pm: name of the group
 *
 * Scalar objects included in the package get sys fs attributes created for
 * them. Nested packages are passed to a function creating a sys fs group per
 * package.
 */
static void handle_acpi_package(union acpi_object *po, char *pm)
{
	int j;
	int count = po->package.count;
	for (j = 0; j < count; j++) {
		union acpi_object *element = po->package.elements + j;
		int copy_size = 0;
		char attr_value[256];	/* strings could be this long */

		switch (element->type) {
		case ACPI_TYPE_INTEGER:
			copy_size = snprintf(attr_value, sizeof(attr_value),
					     "%d", (int)element->integer.value);
			add_sysfs_attribute(attr_value, pm, count, j);
			break;

		case ACPI_TYPE_STRING:
			copy_size = min(element->string.length,
					sizeof(attr_value) - 1);
			memcpy(attr_value, element->string.pointer, copy_size);
			attr_value[copy_size] = '\0';
			add_sysfs_attribute(attr_value, pm, count, j);
			break;

		case ACPI_TYPE_PACKAGE:
			handle_nested_acpi_package(element, pm, count, j);
			break;

		default:
			printk(MY_ERR "ignoring type %d\n", element->type);
			break;
		}
	}
}


/*
 * add_acpi_method() evaluate an ACPI method and create sysfs attributes.
 *
 * @device: ACPI device
 * @pm: name of the method to evaluate
 */
static void add_acpi_method(struct acpi_device *device, char *pm)
{
	acpi_status status;
	struct acpi_buffer output;
	union acpi_object *po;

	output.length = ACPI_ALLOCATE_BUFFER;
	output.pointer = NULL;

	status = acpi_evaluate_object(device->handle, pm, NULL, &output);

	if (!ACPI_SUCCESS(status)) {
		printk(MY_ERR "failed to retrieve %s (%d)\n", pm, status);
		return;
	}

	po = output.pointer;

	if (po->type != ACPI_TYPE_PACKAGE)
		printk(MY_ERR "%s is not a package, ignored\n", pm);
	else
		handle_acpi_package(po, pm);

	/* Need to export a couple of variables to chromeos.c */
	if (!strncmp(pm, "CHNV", 4))
		handle_single_int(po, &chromeos_acpi_chnv);
	else if (!strncmp(pm, "CHSW", 4))
		handle_single_int(po, &chromeos_acpi_chsw);

	kfree(output.pointer);
}

/*
 * chromeos_process_mlst() Evaluate the MLST method and add methods listed
 *                         in the response.
 *
 * @device: ACPI device
 *
 * Returns: 0 if successful, non-zero if error.
 */
static int chromeos_process_mlst(struct acpi_device *device)
{
	acpi_status status;
	struct acpi_buffer output;
	union acpi_object *po;
	int j;

	output.length = ACPI_ALLOCATE_BUFFER;
	output.pointer = NULL;

	status = acpi_evaluate_object(device->handle, MLST_METHOD, NULL,
				      &output);
	if (!ACPI_SUCCESS(status)) {
		pr_debug(MY_LOGPREFIX "failed to retrieve MLST (%d)\n",
			 status);
		return 1;
	}

	po = output.pointer;
	if (po->type != ACPI_TYPE_PACKAGE) {
		printk(MY_ERR MLST_METHOD "is not a package, ignored\n");
		kfree(output.pointer);
		return -EINVAL;
	}

	for (j = 0; j < po->package.count; j++) {
		union acpi_object *element = po->package.elements + j;
		int copy_size = 0;
		char method[ACPI_NAME_SIZE + 1];

		if (element->type == ACPI_TYPE_STRING) {
			copy_size = min((int)element->string.length,
					ACPI_NAME_SIZE);
			memcpy(method, element->string.pointer, copy_size);
			method[copy_size] = '\0';
			add_acpi_method(device, method);
		} else {
			pr_debug(MY_LOGPREFIX "ignoring type %d\n",
				 element->type);
		}
	}

	kfree(output.pointer);
	return 0;
}

static int chromeos_device_add(struct acpi_device *device)
{
	int i;

	/* Attempt to add methods by querying the device's MLST method
	 * for the list of methods. */
	if (!chromeos_process_mlst(device))
		return 0;

	printk(MY_INFO "falling back to default list of methods\n");
	for (i = 0; i < ARRAY_SIZE(default_methods); i++)
		add_acpi_method(device, default_methods[i]);
	return 0;
}

static int chromeos_device_remove(struct acpi_device *device, int type)
{
	return 0;
}

static int __init chromeos_acpi_init(void)
{
	int ret = 0;

	if (acpi_disabled)
		return -ENODEV;

	chromeos_acpi.p_dev = platform_device_register_simple("chromeos_acpi",
							      -1, NULL, 0);
	if (IS_ERR(chromeos_acpi.p_dev)) {
		printk(MY_ERR "unable to register platform device\n");
		return PTR_ERR(chromeos_acpi.p_dev);
	}

	ret = acpi_bus_register_driver(&chromeos_acpi_driver);
	if (ret < 0) {
		printk(MY_ERR "failed to register driver (%d)\n", ret);
		platform_device_unregister(chromeos_acpi.p_dev);
		chromeos_acpi.p_dev = NULL;
		return ret;
	}

	chromeos_acpi_available = true;

	return 0;
}
subsys_initcall(chromeos_acpi_init);
