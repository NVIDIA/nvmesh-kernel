/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_ADMIN_CHANNEL_H
#define NVMEIBC_ADMIN_CHANNEL_H

#include "kr_incs.h"
#include "nvmeibc_channel.h"
#include "nvmeibc_types.h"
#include "nvmeibc_disk_locks.h"
#include "nvmeibs_types.h"
#include "nvmeib_version_shared.h"
#include "nvmeibc_msgs_shared.h"
#include "vex/nvmeibc_vex_shared.h"

struct nvmeibc_admin_channel {
	/* our base channel */
	struct nvmeibc_channel base;
	/* channel's arnic */
	struct nvmeibc_admin_rnic *arnic;
	/* the controller version */
	union nvmeib_version link_version;
	/* controler cockie for us */
	u64 cid;
	/* the page size of the remote machine that holds the rnic */
	int cntr_page_size;
	/* the actul number of the remote admin nic's disks the user will access */
	int n_disks;
	/* the admin channel admins remote io nics and remote disks */
	/* the rionics list holds a list of struct nvmeibc_io_rnic */
	struct list_head rionics;
	/* for each needed remote io nic from this channel
	   we inc the n_rionics_used.  if at the end the channel has
	   n_rionics_used == 0 we drop the channel
	   note that rionic is counted only once for admin channel.
	   no one have more than one channel.
	*/
	int n_rionics_used;
	/* true if we allocated resources through it.
	   the main channel is the channel through which no-rdda transaction
	   will be carried out. For each disk we have only one admin channel */
	bool is_main;
	/* work queue for channel removal works */
	struct workq_struct *remove_wq;
	atomic_t wq_high_pri_cnt;
	/* segment locks */
	struct nvmeibc_disk_segments_locks segments_locks_remote;
	/* periodic */
	spinlock_t periodics_guard;
	struct list_head periodics;
	atomic_t pending_start_ioch_wq;

	const struct vex_ops *vex_ops[vex_ach_ops_num];
	const struct vex_ops *vex_nrio_ops[vex_nrch_ops_num];
};

struct nvmeibc_disk;
struct disk_wrapper {
	struct nvmeibc_disk *disk;
	struct list_head link;
};

struct admin_periodic;
struct admin_periodic {
	/* return value > 0 remove entry
	   return value < 0 remove and free entry
	*/
	int (*on_periodic)(void *arg, unsigned long t);
	void *on_periodic_arg;
	/* fill in by the channel for quick removal */
	void (*remove_periodic)(struct nvmeibc_admin_channel *ch,
							struct admin_periodic *p);
	struct nvmeibc_admin_channel *ch;
	struct list_head link;
};

static inline struct nvmeibc_admin_channel *c_to_ac(struct nvmeibc_channel *ch)
{
	return container_of(ch, struct nvmeibc_admin_channel, base);
}

static inline struct nvmeibc_admin_channel *rionics_to_ac(
	struct list_head *rionics)
{
	return container_of(rionics, struct nvmeibc_admin_channel, rionics);
}

struct nvmeibc_cinst_params_core;
int nvmeibc_admin_channel_init(
	const struct nvmeibc_cinst_params_core *p, 
	struct nvmeibc_admin_channel *ch);
void nvmeibc_admin_channel_free(struct nvmeibc_admin_channel *ch);
int nvmeibc_admin_channel_add_work(struct nvmeibc_admin_channel *ch,
	struct workqe_struct *work);
int nvmeibc_admin_channel_add_low_pri_work(struct nvmeibc_admin_channel *ch,
	struct workqe_struct *work);
void nvmeibc_admin_channel_add_periodic(struct nvmeibc_admin_channel *ch,
	struct admin_periodic *p);
void nvmeibc_admin_channel_remove_periodic(struct nvmeibc_admin_channel *ch,
	struct admin_periodic *p);
void nvmeibc_admin_channel_exec_periodic(struct nvmeibc_admin_channel *ch,
	unsigned long t);

#endif

