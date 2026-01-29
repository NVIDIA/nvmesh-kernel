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
	NVMEIB_DEVCAP_RDDA |
	NVMEIB_DEVCAP_SRQ |
	NVMEIB_DEVCAP_SRQ_LAST_WQE |
	NVMEIB_DEVCAP_ATOMICS_REQ |
	NVMEIB_DEVCAP_ATOMICS_RESP |
	NVMEIB_DEVCAP_MASKED_ATOMICS_REQ |
	NVMEIB_DEVCAP_MASKED_ATOMICS_RESP  |
	NVMEIB_DEVCAP_RD_ATOM_16 |
	NVMEIB_DEVCAP_RD_ATOM_8;

#define MLX5_MAX_RDDA_BLACKLIST 32
static char *mlx5_rdda_blacklist[MLX5_MAX_RDDA_BLACKLIST] = {
	[0] = "16.30.1004",
	[1] = "16.29.2002",
	[2] = "16.29.1016",
	[3 ... 31] = "",
};
module_param_array(mlx5_rdda_blacklist, charp, NULL, 0444);
MODULE_PARM_DESC(mlx5_rdda_blacklist, "Mellanox 5 Firmware Blacklist for RDDA");

/**
 * Copied from Mellanox mlx5
 *
 */

/* client side */

static ssize_t nvmeib_mlx5_get_qp_usage(struct ib_device *ib_dev, struct ib_qp *ib_qp,
	enum nvmeib_cnt_mem_type mem_type)
{
	return _nvmeib_mlx5_get_qp_usage(ib_dev, ib_qp, mem_type);
}

static ssize_t nvmeib_mlx5_get_srq_usage(struct ib_device *ib_dev, struct ib_srq *ib_srq,
	enum nvmeib_cnt_mem_type mem_type)
{
	return _nvmeib_mlx5_get_srq_usage(ib_dev, ib_srq, mem_type);
}

static ssize_t nvmeib_mlx5_get_cq_usage(struct ib_device *ib_dev, struct ib_cq *ib_cq,
	enum nvmeib_cnt_mem_type mem_type)
{
	return _nvmeib_mlx5_get_cq_usage(ib_dev, ib_cq, mem_type);
}

static ssize_t nvmeib_mlx5_get_mr_usage(struct ib_device *ib_dev, struct ib_mr *ib_mr,
	enum nvmeib_cnt_mem_type mem_type)
{
	return _nvmeib_mlx5_get_mr_usage(ib_dev, ib_mr, mem_type);
}

int nvmeib_mlx5_check_rdda_fw(struct ib_device *ib_dev);
int nvmeib_mlx5_check_rdda_fw(struct ib_device *ib_dev)
{
	struct ib_device_attr attrs;
	int rv;
	struct {
		u16 subminor;
		u16 minor;
		u16 major;
		u16 reserved;
	} __attribute__((packed)) *ptr_fw_fields = (void *)&attrs.fw_ver;
	int i;
	
	if ((rv = nvmeib_query_device(ib_dev, &attrs)) < 0) {
		_NE(err_nvmeib_mlx5_fw_supports_rdda, 
		    "nvmeib_query_device failed (@RV) for @IB_DEVICE",
		    rv, ib_dev->name);
		goto out;
	}
	for (i = 0; i < MLX5_MAX_RDDA_BLACKLIST; i++) {
		int major, minor, subminor;
		if (!strlen(mlx5_rdda_blacklist[i]))
			continue;
		if (sscanf(mlx5_rdda_blacklist[i], "%d.%d.%d",
			&major, &minor, &subminor) != 3) {
			_NI(trace_3_nvmeib_mlx5_fw_supports_rdda,
			    "Blacklist FW string @STR has unexpected format",
				mlx5_rdda_blacklist[i]);
			continue;
		}
		if (major == ptr_fw_fields->major &&
			minor == ptr_fw_fields->minor &&
			subminor == ptr_fw_fields->subminor) {
			_NI(trace_nvmeib_mlx5_fw_supports_rdda,
				"Device: @IB_DEVICE has blacklisted RDDA FW @MAJOR.@MINOR.@SUBMINOR", 
				ib_dev->name, ptr_fw_fields->major, ptr_fw_fields->minor, ptr_fw_fields->subminor);
			rv = -ENOTSUPP;
			goto out;
		}
	}
	_NI(trace_2_nvmeib_mlx5_fw_supports_rdda,
	    "Device: @IB_DEVICE has compatible RDDA FW @MAJOR.@MINOR.@SUBMINOR", 
		ib_dev->name, ptr_fw_fields->major, ptr_fw_fields->minor, ptr_fw_fields->subminor);
	rv = 0;

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_mlx5_check_rdda_fw);

#endif //#if !IB_MLX5

static struct nvmeib_device_ops mlx5 = {
	.module = THIS_MODULE,
	.get_qp_usage = nvmeib_mlx5_get_qp_usage,
	.get_srq_usage = nvmeib_mlx5_get_srq_usage,
	.get_cq_usage = nvmeib_mlx5_get_cq_usage,
	.get_mr_usage = nvmeib_mlx5_get_mr_usage,
	.check_rdda_fw = nvmeib_mlx5_check_rdda_fw,
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
