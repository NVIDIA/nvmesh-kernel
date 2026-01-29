/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_IB_NET_ADMIN_H
#define NVMEIBC_IB_NET_ADMIN_H

#include "nvmeibc_ib_net.h"

struct nvmeibc_ib_net_admin {
	struct nvmeibc_ib_net base;
};

static inline struct nvmeibc_ib_net_admin *in_to_ina(struct nvmeibc_ib_net *net)
{
	return container_of(net, struct nvmeibc_ib_net_admin, base);
}

struct nvmeibc_login_request;
struct nvmeibc_volume_req_info;
int nvmeibc_ib_net_admin_alloc(struct nvmeibc_ib_net_admin *net,
	struct nvmeibc_ib_net_params *params, struct nvmeibc_login_request *lreq);
void nvmeibc_ib_net_admin_free(struct nvmeibc_ib_net_admin *net);

#endif
