/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include "nvmeib_nvme.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_ib_net_admin.h"
#include "nvmeibc_ib_admin_channel.h"
#include "nvmeibc_volume.h"
#include "nvmeibc_disk.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeib_utils.h"
#include "nvmeibc_main.h"


#define __FIN FINS(net->base.ioch->name)
#define __FOUT FOUTS(net->base.ioch->name)

#define __NFIN NFINS(net->base.ioch->name)
#define __NFOUT NFOUTS(net->base.ioch->name)

int nvmeibc_ib_net_admin_alloc(struct nvmeibc_ib_net_admin *net,
	struct nvmeibc_ib_net_params *params, struct nvmeibc_login_request *lreq)
{
	return nvmeibc_ib_net_alloc(&net->base, params, lreq);
}

void nvmeibc_ib_net_admin_free(struct nvmeibc_ib_net_admin *net)
{
	__NFIN;
	nvmeibc_ib_net_free(&net->base);
	__NFOUT;
}
