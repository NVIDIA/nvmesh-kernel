/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#if 0

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeibc_module_input_char_device_inc_c

#define NVMEIBC_DRIVER_NAME "nvmeibc_chr"
static int major_number;
static DEFINE_MUTEX(char_dev_guard);

static int nvmeibc_chr_open(struct inode *inode, struct file *file)
{
	int rv;
	long minor = iminor(inode);
	file->private_data = (void *)minor;
	rv = nonseekable_open(inode, file);
	return rv;
}

static int nvmeibc_chr_release(struct inode *inode, struct file *filp)
{
	return 0;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35))
static int nvmeibc_chr_ioctl(
	struct inode *i, struct file *f, unsigned int cmd, unsigned long arg)
#else
static long nvmeibc_chr_ioctl(
	struct file *f, unsigned int cmd, unsigned long arg)
#endif
{
	int rc = -ENXIO;

	mutex_lock(&char_dev_guard);
	/* Daniel please fill in the ioctl support
	switch (cmd){
	}
	*/
	mutex_unlock(&char_dev_guard);
	return rc;
}

static const struct file_operations nvmeibc_chr_fops = {
		.owner = THIS_MODULE,
		.open = nvmeibc_chr_open,
		.release = nvmeibc_chr_release,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35))
		.ioctl = nvmeibc_chr_ioctl,
#else
		.unlocked_ioctl = nvmeibc_chr_ioctl,
#endif
};

static int char_device_create(void)
{
	int rv;

	NFIN;
	major_number = register_chrdev(
		0, NVMEIBC_DRIVER_NAME, &nvmeibc_chr_fops);
	if (major_number < 0) {
		rv = major_number;
		_NT(char_device_create_1,
			"Failed to register major number for nvmeibc char device: @INT",
			rv);
		goto out;
	}
	else
		rv = 0;
	_NT(char_device_create_2,
		"nvmeibc char device major number is @INT", major_number);

out:
	NFOUT;
	return rv;
}

static int char_device_destory(void)
{
	if (major_number >= 0) {
		unregister_chrdev(major_number, NVMEIBC_DRIVER_NAME);
		_NT(char_device_destory_1,
			"nvmeibc char device with major number @INT destroyed",
			major_number);
	}
	return 0;
}

#pragma pop_macro("__FILE_LITERAL__")

#endif