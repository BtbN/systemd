/*
 * namedev.c
 *
 * Userspace devfs
 *
 * Copyright (C) 2003 Greg Kroah-Hartman <greg@kroah.com>
 *
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the
 *	Free Software Foundation version 2 of the License.
 * 
 *	This program is distributed in the hope that it will be useful, but
 *	WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *	General Public License for more details.
 * 
 *	You should have received a copy of the GNU General Public License along
 *	with this program; if not, write to the Free Software Foundation, Inc.,
 *	675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#undef DEBUG

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#include "list.h"
#include "udev.h"
#include "udev_version.h"
#include "namedev.h"
#include "libsysfs/libsysfs.h"

#define TYPE_LABEL	"LABEL"
#define TYPE_NUMBER	"NUMBER"
#define TYPE_TOPOLOGY	"TOPOLOGY"
#define TYPE_REPLACE	"REPLACE"

static LIST_HEAD(config_device_list);

static void dump_dev(struct config_device *dev)
{
	switch (dev->type) {
	case KERNEL_NAME:
		dbg("KERNEL name ='%s'"
			" owner = '%s', group = '%s', mode = '%#o'",
			dev->attr.name, 
			dev->attr.owner, dev->attr.group, dev->attr.mode);
		break;
	case LABEL:
		dbg("LABEL name = '%s', bus = '%s', sysfs_file = '%s', sysfs_value = '%s'"
			" owner = '%s', group = '%s', mode = '%#o'",
			dev->attr.name, dev->bus, dev->sysfs_file, dev->sysfs_value,
			dev->attr.owner, dev->attr.group, dev->attr.mode);
		break;
	case NUMBER:
		dbg("NUMBER name = '%s', bus = '%s', id = '%s'"
			" owner = '%s', group = '%s', mode = '%#o'",
			dev->attr.name, dev->bus, dev->id,
			dev->attr.owner, dev->attr.group, dev->attr.mode);
		break;
	case TOPOLOGY:
		dbg("TOPOLOGY name = '%s', bus = '%s', place = '%s'"
			" owner = '%s', group = '%s', mode = '%#o'",
			dev->attr.name, dev->bus, dev->place,
			dev->attr.owner, dev->attr.group, dev->attr.mode);
		break;
	case REPLACE:
		dbg("REPLACE name = %s, kernel_name = %s"
			" owner = '%s', group = '%s', mode = '%#o'",
			dev->attr.name, dev->kernel_name,
			dev->attr.owner, dev->attr.group, dev->attr.mode);
		break;
	default:
		dbg("Unknown type of device!");
	}
}

#define copy_var(a, b, var)		\
	if (b->var)			\
		a->var = b->var;

#define copy_string(a, b, var)		\
	if (strlen(b->var))		\
		strcpy(a->var, b->var);

static int add_dev(struct config_device *new_dev)
{
	struct list_head *tmp;
	struct config_device *tmp_dev;

	/* loop through the whole list of devices to see if we already have
	 * this one... */
	list_for_each(tmp, &config_device_list) {
		struct config_device *dev = list_entry(tmp, struct config_device, node);
		if (strcmp(dev->attr.name, new_dev->attr.name) == 0) {
			/* the same, copy the new info into this structure */
			copy_var(dev, new_dev, type);
			copy_var(dev, new_dev, attr.mode);
			copy_string(dev, new_dev, bus);
			copy_string(dev, new_dev, sysfs_file);
			copy_string(dev, new_dev, sysfs_value);
			copy_string(dev, new_dev, id);
			copy_string(dev, new_dev, place);
			copy_string(dev, new_dev, kernel_name);
			copy_string(dev, new_dev, attr.owner);
			copy_string(dev, new_dev, attr.group);
			return 0;
		}
	}

	/* not found, lets create a new structure, and add it to the list */
	tmp_dev = malloc(sizeof(*tmp_dev));
	if (!tmp_dev)
		return -ENOMEM;
	memcpy(tmp_dev, new_dev, sizeof(*tmp_dev));
	list_add(&tmp_dev->node, &config_device_list);
	//dump_dev(tmp_dev);
	return 0;
}

static void dump_dev_list(void)
{
	struct list_head *tmp;

	list_for_each(tmp, &config_device_list) {
		struct config_device *dev = list_entry(tmp, struct config_device, node);
		dump_dev(dev);
	}
}

static int get_value(const char *left, char **orig_string, char **ret_string)
{
	char *temp;
	char *string = *orig_string;

	/* eat any whitespace */
	while (isspace(*string))
		++string;

	/* split based on '=' */
	temp = strsep(&string, "=");
	if (strcasecmp(temp, left) == 0) {
		/* got it, now strip off the '"' */
		while (isspace(*string))
			++string;
		if (*string == '"')
			++string;
		temp = strsep(&string, "\"");
		*ret_string = temp;
		*orig_string = string;
		return 0;
	}
	return -ENODEV;
}
	
static int get_pair(char **orig_string, char **left, char **right)
{
	char *temp;
	char *string = *orig_string;

	/* eat any whitespace */
	while (isspace(*string))
		++string;

	/* split based on '=' */
	temp = strsep(&string, "=");
	*left = temp;

	/* take the right side and strip off the '"' */
	while (isspace(*string))
		++string;
	if (*string == '"')
		++string;
	temp = strsep(&string, "\"");
	*right = temp;
	*orig_string = string;
	
	return 0;
}

static int namedev_init_config(void)
{
	char filename[255];
	char line[255];
	char *temp;
	char *temp2;
	char *temp3;
	FILE *fd;
	int retval = 0;
	struct config_device dev;

	strcpy(filename, NAMEDEV_CONFIG_ROOT NAMEDEV_CONFIG_FILE);
	dbg("opening %s to read as permissions config", filename);
	fd = fopen(filename, "r");
	if (fd == NULL) {
		dbg("Can't open %s", filename);
		return -ENODEV;
	}

	/* loop through the whole file */
	while (1) {
		/* get a line */
		temp = fgets(line, sizeof(line), fd);
		if (temp == NULL)
			break;

		dbg("read %s", temp);

		/* eat the whitespace at the beginning of the line */
		while (isspace(*temp))
			++temp;

		/* no more line? */
		if (*temp == 0x00)
			continue;

		/* see if this is a comment */
		if (*temp == COMMENT_CHARACTER)
			continue;

		memset(&dev, 0x00, sizeof(struct config_device));

		/* parse the line */
		temp2 = strsep(&temp, ",");
		if (strcasecmp(temp2, TYPE_LABEL) == 0) {
			/* label type */
			dev.type = LABEL;

			/* BUS="bus" */
			retval = get_value("BUS", &temp, &temp3);
			if (retval)
				continue;
			strcpy(dev.bus, temp3);

			/* file="value" */
			temp2 = strsep(&temp, ",");
			retval = get_pair(&temp, &temp2, &temp3);
			if (retval)
				continue;
			strcpy(dev.sysfs_file, temp2);
			strcpy(dev.sysfs_value, temp3);

			/* NAME="new_name" */
			temp2 = strsep(&temp, ",");
			retval = get_value("NAME", &temp, &temp3);
			if (retval)
				continue;
			strcpy(dev.attr.name, temp3);

			dbg("LABEL name = '%s', bus = '%s', sysfs_file = '%s', sysfs_value = '%s'", dev.attr.name, dev.bus, dev.sysfs_file, dev.sysfs_value);
		}

		if (strcasecmp(temp2, TYPE_NUMBER) == 0) {
			/* number type */
			dev.type = NUMBER;

			/* BUS="bus" */
			retval = get_value("BUS", &temp, &temp3);
			if (retval)
				continue;
			strcpy(dev.bus, temp3);

			/* ID="id" */
			temp2 = strsep(&temp, ",");
			retval = get_value("id", &temp, &temp3);
			if (retval)
				continue;
			strcpy(dev.id, temp3);

			/* NAME="new_name" */
			temp2 = strsep(&temp, ",");
			retval = get_value("NAME", &temp, &temp3);
			if (retval)
				continue;
			strcpy(dev.attr.name, temp3);

			dbg("NUMBER name = '%s', bus = '%s', id = '%s'", dev.attr.name, dev.bus, dev.id);
		}

		if (strcasecmp(temp2, TYPE_TOPOLOGY) == 0) {
			/* number type */
			dev.type = TOPOLOGY;

			/* BUS="bus" */
			retval = get_value("BUS", &temp, &temp3);
			if (retval)
				continue;
			strcpy(dev.bus, temp3);

			/* PLACE="place" */
			temp2 = strsep(&temp, ",");
			retval = get_value("place", &temp, &temp3);
			if (retval)
				continue;
			strcpy(dev.place, temp3);

			/* NAME="new_name" */
			temp2 = strsep(&temp, ",");
			retval = get_value("NAME", &temp, &temp3);
			if (retval)
				continue;
			strcpy(dev.attr.name, temp3);

			dbg("TOPOLOGY name = '%s', bus = '%s', place = '%s'", dev.attr.name, dev.bus, dev.place);
		}

		if (strcasecmp(temp2, TYPE_REPLACE) == 0) {
			/* number type */
			dev.type = REPLACE;

			/* KERNEL="kernel_name" */
			retval = get_value("KERNEL", &temp, &temp3);
			if (retval)
				continue;
			strcpy(dev.kernel_name, temp3);

			/* NAME="new_name" */
			temp2 = strsep(&temp, ",");
			retval = get_value("NAME", &temp, &temp3);
			if (retval)
				continue;
			strcpy(dev.attr.name, temp3);
			dbg("REPLACE name = %s, kernel_name = %s", dev.attr.name, dev.kernel_name);
		}

		retval = add_dev(&dev);
		if (retval) {
			dbg("add_dev returned with error %d", retval);
			goto exit;
		}
	}

exit:
	fclose(fd);
	return retval;
}	


static int namedev_init_permissions(void)
{
	char filename[255];
	char line[255];
	char *temp;
	char *temp2;
	FILE *fd;
	int retval = 0;
	struct config_device dev;

	strcpy(filename, NAMEDEV_CONFIG_ROOT NAMEDEV_CONFIG_PERMISSION_FILE);
	dbg("opening %s to read as permissions config", filename);
	fd = fopen(filename, "r");
	if (fd == NULL) {
		dbg("Can't open %s", filename);
		return -ENODEV;
	}

	/* loop through the whole file */
	while (1) {
		/* get a line */
		temp = fgets(line, sizeof(line), fd);
		if (temp == NULL)
			break;

		dbg("read %s", temp);

		/* eat the whitespace at the beginning of the line */
		while (isspace(*temp))
			++temp;

		/* no more line? */
		if (*temp == 0x00)
			continue;

		/* see if this is a comment */
		if (*temp == COMMENT_CHARACTER)
			continue;

		memset(&dev, 0x00, sizeof(dev));

		/* parse the line */
		temp2 = strsep(&temp, ":");
		strncpy(dev.attr.name, temp2, sizeof(dev.attr.name));

		temp2 = strsep(&temp, ":");
		strncpy(dev.attr.owner, temp2, sizeof(dev.attr.owner));

		temp2 = strsep(&temp, ":");
		strncpy(dev.attr.group, temp2, sizeof(dev.attr.owner));

		dev.attr.mode = strtol(temp, NULL, 8);

		dbg("name = %s, owner = %s, group = %s, mode = %#o", dev.attr.name, dev.attr.owner, dev.attr.group, dev.attr.mode);
		retval = add_dev(&dev);
		if (retval) {
			dbg("add_dev returned with error %d", retval);
			goto exit;
		}
	}

exit:
	fclose(fd);
	return retval;
}	

static int get_default_mode(struct sysfs_class_device *class_dev)
{
	/* just default everyone to rw for the world! */
	return 0666;
}


static int get_attr(struct sysfs_class_device *class_dev, struct device_attr *attr)
{
	struct list_head *tmp;
	int retval = 0;
	int found;

	attr->mode = -1;
	if (class_dev->sysdevice) {
		dbg("class_dev->sysdevice->directory->path = '%s'", class_dev->sysdevice->directory->path);
		dbg("class_dev->sysdevice->bus_id = '%s'", class_dev->sysdevice->bus_id);
	} else {
		dbg("class_dev->name = '%s'", class_dev->name);
	}
	list_for_each(tmp, &config_device_list) {
		struct config_device *dev = list_entry(tmp, struct config_device, node);
		switch (dev->type) {
		case LABEL:
			{
			char *temp;

			dbg("LABEL: match file '%s' with value '%s'", dev->sysfs_file, dev->sysfs_value);
			/* try to find the attribute in the class device directory */
			temp = sysfs_get_value_from_attributes(class_dev->directory->attributes, dev->sysfs_file);
			if (temp)
				goto label_found;

			/* look in the class device device directory if present */
			if (class_dev->sysdevice) {
				temp = sysfs_get_value_from_attributes(class_dev->sysdevice->directory->attributes, dev->sysfs_file);
				if (temp)
					goto label_found;
			}

			/* bah, let's go backwards up a level to see if the device is there,
			 * as block partitions don't point to the physical device.  Need to fix that
			 * up in the kernel...
			 */
			if (strstr(class_dev->directory->path, "block")) {
				dbg("looking at block device...");
				if (isdigit(class_dev->directory->path[strlen(class_dev->directory->path)-1])) {
					char path[SYSFS_PATH_MAX];
					struct sysfs_class_device *class_dev_parent;

					dbg("really is a partition...");
					strcpy(path, class_dev->directory->path);
					temp = strrchr(path, '/');
					*temp = 0x00;
					dbg("looking for a class device at '%s'", path);
					class_dev_parent = sysfs_open_class_device(path);
					if (class_dev_parent == NULL) {
						dbg ("sysfs_open_class_device failed");
						continue;
					}
					dbg("class_dev_parent->name = %s", class_dev_parent->name);

					/* try to find the attribute in the class device directory */
					temp = sysfs_get_value_from_attributes(class_dev_parent->directory->attributes, dev->sysfs_file);
					if (temp) {
						//sysfs_close_class_device(class_dev_parent);
						goto label_found;
					}

					/* look in the class device device directory if present */
					if (class_dev_parent->sysdevice) {
						temp = sysfs_get_value_from_attributes(class_dev_parent->sysdevice->directory->attributes, dev->sysfs_file);
						if (temp) {
							// sysfs_close_class_device(class_dev_parent);
							goto label_found;
						}
					}
					
				}
			}
			continue;

label_found:
			temp[strlen(temp)-1] = 0x00;
			dbg("file '%s' found with value '%s' compare with '%s'", dev->sysfs_file, temp, dev->sysfs_value);
			if (strcmp(dev->sysfs_value, temp) != 0)
				continue;

			strcpy(attr->name, dev->attr.name);
			if (isdigit(class_dev->directory->path[strlen(class_dev->directory->path)-1])) {
				temp[0] = class_dev->directory->path[strlen(class_dev->directory->path)-1];
				temp[1] = 0x00;
				strcat(attr->name, temp);
			}
			if (dev->attr.mode != 0) {
				attr->mode = dev->attr.mode;
				strcpy(attr->owner, dev->attr.owner);
				strcpy(attr->group, dev->attr.group);
			}
			dbg("file '%s' with value '%s' becomes '%s' - owner = %s, group = %s, mode = %#o",
				dev->sysfs_file, dev->sysfs_value, attr->name, 
				dev->attr.owner, dev->attr.group, dev->attr.mode);
			goto done;
			break;
			}
		case NUMBER:
			{
			char path[SYSFS_PATH_MAX];
			char *temp;

			found = 0;
			if (!class_dev->sysdevice)
				continue;
			strcpy(path, class_dev->sysdevice->directory->path);
			temp = strrchr(path, '/');
			dbg("NUMBER path = '%s'", path);
			dbg("NUMBER temp = '%s' id = '%s'", temp, dev->id);
			if (strstr(temp, dev->id) != NULL) {
				found = 1;
			} else {
				*temp = 0x00;
				temp = strrchr(path, '/');
				dbg("NUMBERY temp = '%s' id = '%s'", temp, dev->id);
				if (strstr(temp, dev->id) != NULL)
					found = 1;
			}
			if (!found)
				continue;

			strcpy(attr->name, dev->attr.name);
			if (dev->attr.mode != 0) {
				attr->mode = dev->attr.mode;
				strcpy(attr->owner, dev->attr.owner);
				strcpy(attr->group, dev->attr.group);
			}
			dbg("device id '%s' becomes '%s' - owner = %s, group = %s, mode = %#o",
				dev->id, attr->name, 
				dev->attr.owner, dev->attr.group, dev->attr.mode);
			goto done;
			break;
			}
		case TOPOLOGY:
			{
			char path[SYSFS_PATH_MAX];
			char *temp;

			if (!class_dev->sysdevice)
				continue;
			found = 0;	
			strcpy(path, class_dev->sysdevice->directory->path);
			temp = strrchr(path, '/');
			dbg("TOPOLOGY path = '%s'", path);
			dbg("TOPOLOGY temp = '%s' place = '%s'", temp, dev->place);
			if (strstr(temp, dev->place) != NULL) {
				found = 1;
			} else {
				*temp = 0x00;
				temp = strrchr(path, '/');
				dbg("TOPOLOGY temp = '%s' place = '%s'", temp, dev->place);
				if (strstr(temp, dev->place) != NULL)
					found = 1;
			}
			if (!found)
				continue;

			strcpy(attr->name, dev->attr.name);
			if (dev->attr.mode != 0) {
				attr->mode = dev->attr.mode;
				strcpy(attr->owner, dev->attr.owner);
				strcpy(attr->group, dev->attr.group);
			}
			dbg("device at '%s' becomes '%s' - owner = %s, group = %s, mode = %#o",
				dev->place, attr->name, 
				dev->attr.owner, dev->attr.group, dev->attr.mode);
			goto done;
			break;
			}
		case REPLACE:
			if (strcmp(dev->kernel_name, class_dev->name) != 0)
				continue;
			strcpy(attr->name, dev->attr.name);
			if (dev->attr.mode != 0) {
				attr->mode = dev->attr.mode;
				strcpy(attr->owner, dev->attr.owner);
				strcpy(attr->group, dev->attr.group);
			}
			dbg("'%s' becomes '%s' - owner = %s, group = %s, mode = %#o",
				dev->kernel_name, attr->name, 
				dev->attr.owner, dev->attr.group, dev->attr.mode);
			goto done;
			break;
		case KERNEL_NAME:
			break;
		default:
			dbg("Unknown type of device '%d'", dev->type);
			break;
		}	
	}
	strcpy(attr->name, class_dev->name);
	
done:
	if (attr->mode == -1) {	
		attr->mode = get_default_mode(class_dev);
		attr->owner[0] = 0x00;
		attr->group[0] = 0x00;
	}
	return retval;
}

int namedev_name_device(struct sysfs_class_device *class_dev, struct device_attr *attr)
{
	int retval;

	retval = get_attr(class_dev, attr);
	if (retval)
		dbg("get_attr failed");

	return retval;
}

int namedev_init(void)
{
	int retval;
	
	retval = namedev_init_config();
	if (retval)
		return retval;

	retval = namedev_init_permissions();
	if (retval)
		return retval;

	dump_dev_list();
	return retval;
}


