# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# nvmeib_common.ko Makefile

# Include common.mk
include $(M)/scripts/common.mk

obj-m += nvmeib_common.o
nvmeib_common-y := nvmeib.o nvmeib_utils.o nvmeib_ib_driver.o nvmeib_wd.o \
	nvmeib_io_stats.o nvmeib_rdma.o \
	nvmeib_msgloop.o nvmeib_srq.o  \
	nvmeib_mcs.o nvmeib_mcs_msg_cache.o nvmeib_version_shared.o nvmeib_version_kernel.o \
	mlx/nvmeib_mlx5.o \
	siw/nvmeib_siw.o \
	nvmeib_numa.o \
	nvmeib_buffer.o \
	proc_epilog.o \
	traces_ids.o \
	nvmeib_json.o \
	nvmeib_jdr.o nvmeib_metrics_jdr.o nvmeib_memmgr_metrics.o nvmeib_error_tags.o \
	nvmeib_completion_noise.o nvmeib_pcpu_wq.o

ifeq ($(call ofed_ver_check,5,1),no)
  # Ofed below 5.1 - support mlx4 driver
  nvmeib_common-y += mlx/nvmeib_mlx4.o
endif

ifeq ($(shell uname -r | cut -f1,2,3 -d.), 3.10.0-693)
	CFLAGS_nvmeib_rdma.o := -DRH_KERN_3_10_0_693
endif

ccflags-y := -DNVMEIB_COMMON -DTRACE_INCLUDE_FILE=\"nvmeibm_trace.h\"

# uncomment only if there is any actual NVMEIB_MEMMGR_METRIC instantiation
#ldflags-y := -T $(M)/common/nvmeib.lds

# Compile gen_events and dictionary for binary tracing
TRACE_MODULE := nvmeibm
TRACE_MODULE_OBJ := $(nvmeib_common-y)

# Do not generate ftrace section
ccflags-remove-y += -pg

# Include trace.mk
include $(M)/scripts/trace.mk

