/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
/* Simulator of lib-udev used by Toma */
#include "os_internal.h"

struct nvmeibt_udev_event_simu {
	const char *dev_file_name;				// Path of the disk
	const struct sandbox_nvme_device *dev;	// May be NULL for non nvme device
	bool action_is_add;
};
struct nvmeibt_udev_simu {
	int n_sent, n_total;
	struct nvmeibt_udev_event_simu events[4];	// A circular buffer to which unit-test env injects udev_events and Toma extracts them 1 by 1
	struct TSB_fd_otherside o;
	const struct sandbox_nvme_device *local_disks;
	unsigned n_disks;
};

// Private API towards unit-test environment
struct nvmeibt_udev_simu *nvmeibt_udev_simu_create(const struct sandbox_nvme_device *local_disks, unsigned n_disks);
struct TSB_fd_otherside  *nvmeibt_udev_simu_connect(struct nvmeibt_udev_simu *u);
void                      nvmeibt_udev_simu_destroy(struct nvmeibt_udev_simu *u);
void nvmeibt_udev_simu_send_disk_event_to_toma(unsigned disk_idx, bool action_is_add);
void nvmeibt_udev_simu_send_sata_event_to_toma(                   bool action_is_add);
bool nvmeibt_udev_simu_did_toma_consume_all_events(void);
