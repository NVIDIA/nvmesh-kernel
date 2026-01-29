/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_UDEV
#define NVMEIBT_UDEV

/* Gets notficiations about insertion and removal of stock nvme drives*/

int  nvmeibt_udev_create( void);		// Constructor
void nvmeibt_udev_destroy(void);		// Destructor
int  nvmeibt_udev_get_fd( void);		// Descriptor to blocking select upon

enum nvmeibt_udev_event_action {
	nvmeibt_udev_none = 0,
	nvmeibt_udev_add,
	nvmeibt_udev_del
};

struct nvmeibt_udev_event {
	const char *dev_file_name;			// Path of the disk
	void *ctx;							// Dont touch, used for lower layer
	enum nvmeibt_udev_event_action action;
};

enum nvmeibt_disk_type {
	NVMEIBT_NVMESH_DISK_TYPE = 19,
	NVMEIBT_NVME_DISK_TYPE,
	NVMEIBT_EXTERNAL_DISK_TYPE,
	NVMEIBT_VIRTUAL_DISK_TYPE,
	NVMEIBT_NOT_SUPPORTED_STOCK_DISK_TYPE
};

// Once 'select' unblocked use method to process event. Call 'put' after 'get'
enum nvmeibt_disk_type nvmeibt_udev_get_event(struct nvmeibt_udev_event *rv);		// Fills 'rv'
void nvmeibt_udev_put_event(struct nvmeibt_udev_event *rv);		// Path rv, returned by 'get'

#endif // #ifndef NVMEIBT_UDEV

