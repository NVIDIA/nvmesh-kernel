# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# nvmeibs.ko Makefile

# Include common.mk
include $(M)/scripts/common.mk

CFLAGS_nvmeibs_um_comm.o := -DCONFIG_NVMEIB_DEBUG -DCONFIG_NVMEIB_FINE
obj-m += nvmeibs.o
nvmeibs-y := nvmeibs_main.o nvmeibs_ib_port.o nvmeibs_client.o nvmeibs_disk.o \
		nvmeibs_net.o nvmeibs_nvme.o nvmeibs_disk_locks.o \
		nvmeibs_toma.o nvmeibs_nordda.o nvmeibs_distribute_intrs.o \
		nvmeibs_mcs.o nvmeibs_serjio_deps.o nvmeibs_serjio.o nvmeibs_serjio_stats.o \
		nvmeibs_um_comm.o nvmeibs_serjio_gen_cmd_handlers.o nvmeibs_client_db.o \
		nvmeibs_capabilities.o \
		nvmeibs_main_gen_cmds.o vex/nvmeibs_vex.o nvmeibs_async_cookies.o traces_ids.o \
		nvmeibs_memmgr_metrics.o

ccflags-y += -DTRACE_INCLUDE_FILE=\"nvmeibs_trace.h\"
ldflags-y += -T $(M)/srv/nvmeibs.lds

# ccflags-y += -DIB_RAM_TESTING
#CFLAGS_nvmeibs_um_comm.o := -DCONFIG_NVMEIB_DEBUG -DCONFIG_NVMEIB_FINE
#CFLAGS_nvmeibs_nvme.o := -DCONFIG_NVMEIB_DEBUG -DCONFIG_NVMEIB_FINE

# Compile gen_events and dictionary for binary tracing
TRACE_MODULE := nvmeibs
TRACE_MODULE_OBJ := $(nvmeibs-y)

# Include trace.mk
include $(M)/scripts/trace.mk
