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

#include "ib_incs.h"

#ifdef KS_HAS_DEVLINK_H
#	include <net/devlink.h>
#endif
#include <linux/netdevice.h>
#include "nvmeib_mlx.h"
#include "nvmeib_ib_driver.h"
#include "nvmeib_utils.h"
#include "nvmeib_public.h"
#include "nvmeibm_trace.h"

#if !defined(IB_MLX5) && !defined(CONFIG_MLX5_INFINIBAND)
# warning MLX5 Driver not supported by this OFED/Kernel version
#else

/* Must be last to override module_{init/exit} */
#include "kr_undef.h"

#ifdef CONFIG_INFINIBAND_ON_DEMAND_PAGING
# undef CONFIG_INFINIBAND_ON_DEMAND_PAGING
#endif

const unsigned long nvmeib_mlx5_dev_caps =
	NVMEIB_DEVCAP_SRQ |
	NVMEIB_DEVCAP_SRQ_LAST_WQE |
	NVMEIB_DEVCAP_ATOMICS_REQ |
	NVMEIB_DEVCAP_ATOMICS_RESP |
	NVMEIB_DEVCAP_MASKED_ATOMICS_REQ |
	NVMEIB_DEVCAP_MASKED_ATOMICS_RESP  |
	NVMEIB_DEVCAP_RD_ATOM_16 |
	NVMEIB_DEVCAP_RD_ATOM_8;

/**
 * Copied from Mellanox mlx5
 *
 */

/* client side */

#endif //#if !IB_MLX5

static struct nvmeib_device_ops mlx5 = {
	.module = THIS_MODULE,
};

int nvmeib_mlx5_init(bool paging_enabled)
{
	return nvmeib_ibdr_hwdev_register(DT_mlx5, "mlx5",
					&mlx5,
					nvmeib_mlx5_dev_caps,
					INT_MAX);
}

void nvmeib_mlx5_cleanup(void)
{
	nvmeib_ibdr_hwdev_unregister(DT_mlx5);
}
