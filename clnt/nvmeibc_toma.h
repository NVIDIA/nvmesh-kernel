/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_TOMA_H
#define NVMEIBC_TOMA_H

#include "nvmeibc_ib_admin_channel.h"

/* Toma IB interface send/recv to/from server */
int  nvmeibc_toma_send_req(struct nvmeibc_ib_admin_channel *ch,
						   struct nvmeibc_disk_toma_cmd *toma_cmd);
void nvmeibc_toma_recv_req(struct nvmeibc_ib_admin_channel *ch,
						   struct volume_server_req *req, const void *e);

/* Toma resources of (disk's main) IB admin channel */
int nvmeibc_toma_create(struct nvmeibc_ib_admin_channel *ch,
						nvmeibc_disk_unsubscribe_toma_comp_callback_t *unsub_cb, 
						nvmeibc_disk_async_subscribe_toma_comp_callback *async_sub_cb);

void nvmeibc_toma_free(struct nvmeibc_ib_admin_channel *ch);

#endif /* NVMEIBC_TOMA_H */
