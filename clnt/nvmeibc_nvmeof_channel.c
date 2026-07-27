#include "kr_incs.h"
#include <linux/syscalls.h>
#include <uapi/linux/pr.h>

#include "nvmeibc_disk.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeibc_types.h"
#include "nvmeibs_types.h"
#include "nvmeibc_block.h"
#include "nvmeib.h"
#include "nvmeib_file.h"
#include "nvmeib_utils.h"
#include "nvmeib_public_procfs.h"
#include "nvmeibc_nvmeof_channel.h"
#include "nvmeibc_main.h"


struct nvmeof_disk {
	struct list_head link;
	struct gendisk *gendisk;
	struct block_device *block_dev;
	int use_count;
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	const struct nvmeibc_cinst_params_core *p;
};

static ssize_t nvmeof_fill(void *arg, char *buf, size_t len)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(
							(const struct nvmeibc_cinst_params_core *)arg);
	struct nvmeof_disk *p;
	size_t filled = 0;

	mutex_lock(&cg->nvmeof.lock);
	list_for_each_entry(p, &cg->nvme_of.disks, link) {
		filled += scnprintf(buf+filled, len - filled, "%s %s\n",
							p->gendisk->disk_name, p->disk_id);
	}
	mutex_unlock(&cg->nvmeof.lock);

	return filled;
}

static void nvmeof_release(struct nvmeof_disk *disk)
{
	list_del(&disk->link);
	if (disk->block_dev)
		blkdev_put(disk->block_dev, FMODE_WRITE|FMODE_READ);
	_NT(nvmeof_release_t1,
		"removing @STR id=@STR", disk->gendisk->disk_name, disk->disk_id);
	kfree(disk);
}

static void nvmeof_new_link(struct nvmeof_disk *nvmeof_disk)
{
	struct list_head *disks = nvmeibc_get_disks(nvmeibc_isnt_params_core2main(nvmeof_disk->p));
	struct nvmeibc_disk *d = NULL;

	_NT(nvmeof_new_link_t1, "nvmeof_new_link(@PTR)", nvmeof_disk);
	list_for_each_entry(d, disks, link) {
		if (strncmp(nvmeof_disk->disk_id, d->name, sizeof nvmeof_disk->disk_id) == 0) {
			_NT(nvmeof_new_link_t2, "connect to disk=%@PTR", d);
			++nvmeof_disk->use_count;
			d->nvmeof_disk = nvmeof_disk;
		}
	}
}

static void nvmeof_link_gone(struct nvmeof_disk *nvmeof_disk)
{
	struct list_head *disks = nvmeibc_get_disks(nvmeibc_isnt_params_core2main(nvmeof_disk->p));
	struct nvmeibc_disk *d = NULL;

	_NT(nvmeof_link_gone_t1, "nvmeof_link_gone(@PTR)", nvmeof_disk);
	list_for_each_entry(d, disks, link) {
		if (d->nvmeof_disk == nvmeof_disk) {
			_NT(nvmeof_link_gone_t2, "unlink from disk=@PTR", d);
			BUG_ON(nvmeof_disk->use_count <= 0);
			--nvmeof_disk->use_count;
			d->nvmeof_disk = NULL;
		}
	}
}

static ssize_t nvmeof_chng(void *arg, char *buf, size_t len)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(
							(const struct nvmeibc_cinst_params_core *)arg);
	char *devname, *serial;
	struct nvmeof_disk *p;
	struct block_device *block_dev;

	devname = strsep(&buf, " \t\n");
	if (devname == NULL)
		return -EINVAL;
	serial = strsep(&buf, " \t\n");

	block_dev = blkdev_get_by_path(devname, FMODE_READ | FMODE_WRITE, NULL);
	if (IS_ERR(block_dev))
		return PTR_ERR(block_dev);
	mutex_lock(&cg->nvme_of.lock);
	list_for_each_entry(p, &cg->nvme_of.disks, link) {
		if (p->block_dev == block_dev)
			break;
	}
	if (serial == NULL || serial[0] == '\0') {
		blkdev_put(block_dev, FMODE_WRITE|FMODE_READ);
		if (&p->link != &cg->nvme_of.disks) {
			nvmeof_link_gone(p);
			if (--p->use_count == 0)
				nvmeof_release(p);
			else
				_NW(nvmeof_chng_w10,
					"Not yet removing @STR id=@STR count=@INT",
					p->gendisk->disk_name, p->disk_id, p->use_count);
		}
		mutex_unlock(&cg->nvme_of.lock);
		if (&p->link == &cg->nvme_of.disks)
			return -ENXIO;
	}
	else {
		mutex_unlock(&cg->nvme_of.lock);
		if (&p->link != &cg->nvme_of.disks)
			return -EEXIST;
		if ((p = kzalloc(sizeof *p, GFP_KERNEL)) == NULL)
			return -ENOMEM;
		p->p = arg;
		p->use_count = 1;
		strlcpy(p->disk_id, serial, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
		p->block_dev = block_dev;
		p->gendisk = p->block_dev->bd_disk;
		mutex_lock(&cg->nvme_of.lock);
		list_add_tail(&p->link, &cg->nvme_of.disks);
		nvmeof_new_link(p);
		_NT(nvmeof_chng_t1,
			"Added @STR id=@STR", p->gendisk->disk_name, p->disk_id);
		mutex_unlock(&cg->nvme_of.lock);
	}
	return len;
}

struct nvmeib_public_procfs_ent *nvmeibc_nvmeof_procfs_init(
	struct proc_dir_entry *nvmeibc_proc_dir, const struct nvmeibc_cinst_params_core *p)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	cg->nvmeof.proc = nvmeib_public_proc_create("nvmeof_disks", nvmeibc_proc_dir,
										nvmeof_fill, nvmeof_chng, p);
	return cg->nvmeof.proc;
}

void nvmeibc_nvmeof_procfs_remove(const struct nvmeibc_cinst_params_core *p)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	struct nvmeof_disk *p;

	nvmeib_public_proc_remove(cg->nvmeof.proc);
	mutex_lock(&cg->nvmeof.lock);
	while (!list_empty(&cg->nvmeof.disks)) {
		p = list_first_entry(&cg->nvmeof.disks, struct nvmeof_disk, link);
		list_del(&p->link);
		mutex_unlock(&cg->nvmeof.lock);
		blkdev_put(p->block_dev, FMODE_WRITE|FMODE_READ);
		kfree(p);
		mutex_lock(&cg->nvmeof.lock);
	}
	cg->nvmeof.proc = NULL;
	mutex_unlock(&cg->nvmeof.lock);
}

struct nvmeof_disk *nvmeibc_get_nvmeof_disk(
	const struct nvmeibc_cinst_params_core *p, const char *serial)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	struct nvmeof_disk *p;
	struct nvmeof_disk *disk_found = NULL;

	mutex_lock(&cg->nvmeof.lock);
	list_for_each_entry(p, &cg->nvmeof.disks, link) {
		if (strcmp(p->disk_id, serial) == 0) {
			++p->use_count;
			disk_found = p;
			break;
		}
	}
	mutex_unlock(&cg->nvmeof.lock);
	_NT(nvmeibc_get_nvmeof_disk_t1,
		"get_nvmeof_disk(@STR)=@PTR", serial, disk_found);
	return disk_found;
}

void nvmeibc_put_nvmeof_disk(struct nvmeof_disk *disk)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(disk->p);
	_NT(nvmeibc_put_nvmeof_disk_t1, "put_nvmeof_disk(@PTR)", disk);
	mutex_lock(&cg->nvmeof.lock);
	if (--disk->use_count == 0)
		nvmeof_release(disk);
	mutex_unlock(&cg->nvmeof.lock);
}




#define NVME_FABRICS_DEV_NAME "/dev/nvme-fabrics"



int nvmeibc_disconnect_nvmeof_device(int instance)
{
	int rv = -1;
	char buf[256];
	int cmd_len;
	struct file* fp;

	_NT(nvmeibc_disconnect_nvmeof_device_t1,
		"disconnecting /dev/nvme@INT", instance);
	cmd_len = snprintf(buf, sizeof(buf),
				   "/sys/class/nvme/nvme%d/delete_controller", instance);

	fp = nvmeib_file_open(buf, O_WRONLY, 0, 0);
	if (!fp) {
		_NW(nvmeibc_disconnect_nvmeof_device_w1,
			"failed openning file @STR", buf);
		goto done;
	}

	// similar to 'echo -n 1 > /sys/class/nvme/nvme2/delete_controller"
	rv = nvmeib_file_write(fp, "1", 1, 0);
	if (rv < 0) {
		_NW(nvmeibc_disconnect_nvmeof_device_w2,
			"Failed writing 1 to @STR", buf);
		goto close;
	}

	rv = 0;

close:
	nvmeib_file_close(fp);
done:
	return rv;
}



static long nvmeibc_connect_nvmeof_disk(struct sockaddr_in* ip_addr, const char* nqn)
{
	ssize_t rv = -1;
	struct file* fp;
	char buf[512];
	int cmd_len;
	char* instance;
	char* p_buf = buf;
	unsigned long res;

	fp = nvmeib_file_open(NVME_FABRICS_DEV_NAME, O_RDWR, 0, 0);
	if (!fp) {
		_NW(nvmeibc_connect_nvmeof_disk,_w1,
			"failed openning file " NVME_FABRICS_DEV_NAME
			" - MAKE SURE nvme_fabrics + nvme_rdma DRIVERS ARE LOADED");
		goto done;
	}

	// e.g. write(3, "nqn=nvme89.0,transport=rdma,traddr=10.1.200.89", 46)
	cmd_len = snprintf(buf, sizeof(buf),
					   "nqn=%s,transport=rdma,traddr=%pI4",
					   nqn, &ip_addr->sin_addr.s_addr);
	_NT(nvmeibc_connect_nvmeof_disk_t1,
		"Trying to connect to NVMEoF target: @STR", buf);
	rv = nvmeib_file_write(fp, buf, cmd_len, 0);
	if (rv < 0) {
		switch (-rv) {
		case ECONNRESET:
			_NW(nvmeibc_connect_nvmeof_disk_w2,
				"Connection to NVMEoF target failed: @IPV4",
				&ip_addr->sin_addr.s_addr);
			goto close;
		case EIO:
			_NW(nvmeibc_connect_nvmeof_disk_w3,
				"Failed connecting to NVMEoF nqn: @STR", nqn);
			goto close;
		default:
			_NW(nvmeibc_connect_nvmeof_disk_w4,
				"Failed connecting to NVMEoF target: @IPV4 @STR",
				&ip_addr->sin_addr.s_addr, nqn);
			goto close;
		}
	}

	// e.g. read(3, "instance=2,cntlid=3\n", 4096) = 20
	rv = nvmeib_file_read(fp, buf, sizeof(buf), 0);
	if (rv < 0) {
		_NW(nvmeibc_connect_nvmeof_disk_w5,
			"Failed getting reply to NVMEoF connect: @IPV4 @STR",
			&ip_addr->sin_addr.s_addr, nqn);
		goto close;
	}
	_NT(nvmeibc_connect_nvmeof_disk_w2_t3,
		"received reply @STR", buf);

	instance = strsep(&p_buf, ",");
	if (!instance) {
		_NW(nvmeibc_connect_nvmeof_disk_w6, "failed parsing reply: @STR", buf);
		goto close;
	}
	// instance points to "instance=2"
	p_buf = instance;
	instance = strsep(&p_buf, "=");
	if (!instance) {
		_NW(nvmeibc_connect_nvmeof_disk_w2_e7,
			"failed parsing reply: @STR", buf);
		goto close;
	}
	instance = p_buf;
	rv = kstrtoul(instance, 10, &res);
	if (rv < 0) {
		_NW(nvmeibc_connect_nvmeof_disk_w8,
			"failed parsing instance value: @STR @STR", buf, instance);
		goto close;
	}

	_NT(nvmeibc_connect_nvmeof_disk_t4,
		"connected NVMEoF target to /dev/nvme@LU", res);
	rv = res;

close:
	nvmeib_file_close(fp);
done:
	return rv;
}




static int nvmeibc_do_nvmeof_registration(long instance_id, int namespace_id, u64 reg_key)
{
	int rv = 0;
	char dev_name[DISK_NAME_LEN + 32];
	struct file* fp;
	struct pr_registration reg = {0};
	int err;
	int dev_not_ready_retries = 10;

	snprintf(dev_name, sizeof(dev_name), "/dev/nvme%lun%d", instance_id, namespace_id);
	_NT(nvmeibc_do_nvmeof_registration_t1, "openning file @STR", dev_name);

retry_device_ready:
	fp = nvmeib_file_open(dev_name, O_RDONLY, 0, &err);
	if (!fp) {
		_NW(nvmeibc_do_nvmeof_registration_w1,
			"failed openning file @STR   err: @INT   retries: @INT",
			dev_name, err, dev_not_ready_retries);
		if ((err == -ENOENT) && dev_not_ready_retries) {
			dev_not_ready_retries--;
			msleep_interruptible(200);
			goto retry_device_ready;
		}
		rv = -1;
		goto done;
	}

	reg.new_key = reg_key;
	_NT(nvmeibc_do_nvmeof_registration_t4,
		"calling registration @STR  key 0x@_X", dev_name, reg.new_key);
	rv = nvmeib_file_ioctl(fp, IOC_PR_REGISTER, (unsigned long)&reg);
	if (rv) {
		_NT(nvmeibc_do_nvmeof_registration_t5,
			"registration failed @STR   @INT", dev_name, rv);
		rv = -1;
		goto close;
	}

close:
	nvmeib_file_close(fp);
done:
	return rv;
}




int nvmeibc_register_nvmeof_disk(struct sockaddr_in* ip_addr, const char* nqn, u64 reg_key,
								 int* p_instance_id, int* p_namespace_id)
{
	int rv = 0;
	long instance_id;
	int namespace_id;

	if (!nqn) {
		rv = -1;
		_NT(nvmeibc_register_nvmeof_disk_t20, "received null nvmeof disk nqn");
		goto done;
	}


	instance_id = nvmeibc_connect_nvmeof_disk(ip_addr, nqn);
	if (instance_id < 0) {
		_NW(nvmeibc_register_nvmeof_disk_w1,
			"failed connecting to nvmeof disk");
		rv = -1;
		goto done;
	}

	namespace_id = 1;	// TODO: this needs to be retrieved from the /dev/nvme%lun* list

	if (p_instance_id) {
		*p_instance_id = instance_id;
	}
	if (p_namespace_id) {
		*p_namespace_id = namespace_id;
	}
	rv = nvmeibc_do_nvmeof_registration(instance_id, namespace_id, reg_key);

done:
	return rv;
}


void nvmeibc_unregister_nvmeof_disk(u64 reg_key, int instance, int namespace)
{
	struct file* fp;
	char disk_name[DISK_NAME_LEN + 32];
	int rv = 0;
	struct pr_clear clear = {0};


	snprintf(disk_name, sizeof(disk_name), "/dev/nvme%dn%d", instance, namespace);
	_NT(nvmeibc_unregister_nvmeof_disk_t1,
		"openning file @STR", disk_name);
	fp = nvmeib_file_open(disk_name, O_RDONLY, 0, 0);
	if (!fp) {
		_NW(nvmeibc_unregister_nvmeof_disk_w1,
			"failed openning file @STR", disk_name);
		rv = -1;
		goto done;
	}

	clear.key = reg_key;
	_NT(nvmeibc_unregister_nvmeof_disk_t2,
		"clearing registration @STR  key @_X", disk_name, clear.key);
	rv = nvmeib_file_ioctl(fp, IOC_PR_CLEAR, (unsigned long)&clear);
	if (rv) {
		_NT(nvmeibc_unregister_nvmeof_disk_t3,
			"clear failed @STR   @INT", disk_name, rv);
	}

	nvmeib_file_close(fp);
done:
	return;
}

