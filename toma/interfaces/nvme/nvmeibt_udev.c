#include "nvmeibt_lib_udev_api.h"
#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_local_disk.h"
#include "nvmeibt_udev.h"

static struct udev 			*udev = NULL;
static struct udev_monitor 	*udev_monitor = NULL;
static int 					udev_fd = 0;

int nvmeibt_udev_create(void)
{
	int rv = -EINVAL;
	udev = udev_new();
	if (!udev) {
		rv = -ENODEV;
		goto _out;
	}

	/* Set up a monitor to monitor nvme devices */
	udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
	if (udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "block", "disk") < 0) {
		rv = -ENOTBLK;
		goto _out;
	}
	if (udev_monitor_enable_receiving(udev_monitor) < 0) {
		rv = -EIO;
		goto _out;
	}

	// Get the file descriptor (fd) for the monitor. This fd will get passed to select()
	udev_fd = udev_monitor_get_fd(udev_monitor);
	rv = udev_fd;			// Daniel, just for prints
_out:
	if (unlikely(rv < 0)) {
		nvmeibt_udev_destroy();
	}
	return rv;
}


void nvmeibt_udev_destroy(void)
{
	if (udev_monitor) {
		udev_monitor_unref(udev_monitor);
		udev_monitor = NULL;
	}
	if (udev) {
		udev_unref(udev);
		udev = NULL;
	}
	udev_fd = 0;
}

int nvmeibt_udev_get_fd(void)
{
	return udev_fd;
}

void nvmeibt_udev_put_event(struct nvmeibt_udev_event *rv)
{
	if (rv->ctx) {
		udev_device_unref(rv->ctx);
	}
	memset(rv, 0, sizeof(*rv));
}

enum nvmeibt_disk_type nvmeibt_udev_get_event(struct nvmeibt_udev_event *rv)
{
	struct udev_device					*dev = udev_monitor_receive_device(udev_monitor);
	enum nvmeibt_disk_type				disk_type;

	memset(rv, 0, sizeof(*rv));
	rv->action = nvmeibt_udev_none;
	disk_type = NVMEIBT_NOT_SUPPORTED_STOCK_DISK_TYPE;
	rv->ctx = dev;
	if (dev) {
		const char *devpath = udev_device_get_devpath(dev);
		const char *syspath = udev_device_get_syspath(dev);
		const char *action = udev_device_get_action(dev);
		const char *subsystem = udev_device_get_subsystem(dev);
		const char *devtype = udev_device_get_devtype(dev);
		const char *devnode = udev_device_get_devnode(dev);
		const char *io_enabled_prop = udev_device_get_property_value(dev, VDISK_UEVENT_IO_ENABLE_KEY);

		if (devnode == NULL) {
			devnode = "NULL";
		}
		if (strlen(devnode) > NVMEIBT_LOCAL_DISK_DEV_FILE_NAME_LEN) {
			N_Wf(mn8ub5v0, "devnode=@STR is too long", devnode);
			goto out;
		}
		rv->dev_file_name = devnode;
		N_Tf(64gs87w, "udev notification action=@STR devnode=@STR devtype=@STR devpath=@STR syspath=@STR subsystem=@STR",
			 action, devnode, devtype, devpath, syspath, subsystem);
		if (!rv->dev_file_name) {
			// [Ronen] We get such a remove event on Ubuntu startup. The BDF does belong to a valid
			//  drive, but this remove is not connected to the reality. Better keep ignoring it
			N_Tf(t_09_tomaudev, "path(devnode)='' - ignoring. Probably another event is on its way");
			goto out;
		}

		disk_type = nvmeibt_local_disk_get_stock_disk_type_by_dev_file_name(rv->dev_file_name, devpath);
		switch (disk_type) {
		case NVMEIBT_NVME_DISK_TYPE:
		case NVMEIBT_EXTERNAL_DISK_TYPE:
			if (!strcmp(action, "add")) {
				rv->action = nvmeibt_udev_add;
			} else if (!strcmp(action, "remove")) {
				N_Tf(jmskq8s, "Usually a remove event arrives after we already 'unbind' and deleted the stock_local_disk");
				rv->action = nvmeibt_udev_del;
			} else {
				N_Tf(gt65r3w, "Skipping irrelevant action");
			}
			break;
		case NVMEIBT_VIRTUAL_DISK_TYPE:
			if (io_enabled_prop) {
				int io_enabled = io_enabled_prop[0] - '0';
				N_Tf(lxlxlx, "Got change event for @STR, is_io_enabled: @INT", devpath, io_enabled);
				rv->action = io_enabled? nvmeibt_udev_add : nvmeibt_udev_del;
			}
			else {
				N_Tf(i876yt4, "Skipping irrelevant action");
			}
			break;
		case NVMEIBT_NVMESH_DISK_TYPE:
		default:
			N_Tf(4vs7j3l, "Udev event path=@STR. Ignoring (not a stock driver)", rv->dev_file_name);
		}

	} else {
		N_Ef(t_0c_tomaudev, "No Device from receive_device(). An error occured");
	}
out:
	return disk_type;
}

