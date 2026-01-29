/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

#include "nvmeib_public.h"
#include "ib_incs.h"
#include "nvmeib_ib_driver.h"
#include "nvmeib_mlx.h"
#include "nvmeib_utils.h"
#include "nvmeibs_test.h"
#include "nvmeibm_trace.h"

#include <rdma/ib_verbs.h>
#include <linux/netdevice.h>
#include <linux/mlx4/device.h>
#include <linux/mlx4/qp.h>
#include <infiniband/hw/mlx4/mlx4_ib.h>

/* Must be last to override module_{init/exit} */
#include "kr_undef.h"

const unsigned long nvmeib_mlx4_dev_caps = 
	NVMEIB_DEVCAP_SRQ | 
	NVMEIB_DEVCAP_SRQ_LAST_WQE |
	NVMEIB_DEVCAP_ATOMICS_REQ |
	NVMEIB_DEVCAP_ATOMICS_RESP |
	NVMEIB_DEVCAP_MASKED_ATOMICS_REQ |
	NVMEIB_DEVCAP_MASKED_ATOMICS_RESP |
	NVMEIB_DEVCAP_RD_ATOM_8;

static struct nvmeib_device_ops mlx4 = {
	.module = THIS_MODULE,
};

int nvmeib_mlx4_init(void)
{
	return nvmeib_ibdr_hwdev_register(DT_mlx4, "mlx4", &mlx4, nvmeib_mlx4_dev_caps, INT_MAX);
}

void nvmeib_mlx4_cleanup(void)
{
	nvmeib_ibdr_hwdev_unregister(DT_mlx4);
}
