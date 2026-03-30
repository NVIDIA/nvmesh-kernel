/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
/* Udev simulator for Toma kower layersandbox unit tests */
#include "os_internal.h"
#include "interfaces/nvme/nvmeibt_udev.h"	// Implementing this public API

struct nvmeibt_udev_simu {
	int n_sent, n_total;
	struct nvmeibt_udev_event events[4];	// A circular buffer to which unit-test env injects udev_events and Toma extracts them 1 by 1
	struct TSB_fd_otherside o;
	const struct sandbox_nvme_device *local_disks;
	unsigned n_disks;
};

// Private API towards unit-test environment
struct nvmeibt_udev_simu *nvmeibt_udev_simu_create(const struct sandbox_nvme_device *local_disks, unsigned n_disks);
struct TSB_fd_otherside  *nvmeibt_udev_simu_connect(struct nvmeibt_udev_simu *u);
void                      nvmeibt_udev_simu_destroy(struct nvmeibt_udev_simu *u);
void nvmeibt_udev_simu_send_disk_event_to_toma(unsigned disk_idx, enum nvmeibt_udev_event_action action);
void nvmeibt_udev_simu_send_sata_event_to_toma(                   enum nvmeibt_udev_event_action action);
bool nvmeibt_udev_simu_did_toma_consume_all_events(void);
