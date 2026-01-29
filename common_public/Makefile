# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# nvmeib_common_public.ko Makefile

# Include common.mk
include $(M)/scripts/common.mk

obj-m := nvmeib_common_public.o \
	nvmeib_common_mlx5_public.o

ifneq ($(BUILD_TCP),)
obj-m += nvmeib_common_siw_public.o
endif

nvmeib_common_public-y := \
	nvmeib_public.o kth/nvmeib_public_kth.o \
	nvmeib_kth_events.o \
	kth/nvmeib_public_kth_sync.o nvmeib_q.o \
	poll/nvmeib_public_intr_poll.o \
	nvmeib_trend.o nvmeib_trace.o nvmeib_rsc_pool.o \
	nvmeib_trace_stress_test.o nvmeib_public_procfs.o nvmeib_public_mmap.o \
	nvmeib_public_raid.o nvmeib_public_keeper.o traces_ids.o nvmeib_event.o

ifeq ($(call ofed_ver_check,5,1),no)
# Ofed below 5.1 - support mlx4 driver
obj-m += nvmeib_common_mlx4_public.o
nvmeib_common_mlx4_public-y := \
	nvmeib_public_mlx4.o traces_ids.o
endif

nvmeib_common_mlx5_public-y := \
	nvmeib_public_mlx5.o traces_ids.o

ifneq ($(BUILD_TCP),)
nvmeib_common_siw_public-y := \
	nvmeib_public_siw.o traces_ids.o
endif

ccflags-y += -DTRACE_INCLUDE_FILE=\"nvmeibp_trace.h\"

#CFLAGS_nvmeibs_serjio.o := -finstrument-functions
#CFLAGS_nvmeib_public.o := -DCONFIG_NVMEIB_DEBUG -DCONFIG_NVMEIB_FINE

# Compile gen_events and dictionary for binary tracing
TRACE_MODULE := nvmeibp
TRACE_MODULE_OBJ := $(nvmeib_common_public-y) $(nvmeib_common_mlx4_public-y) $(nvmeib_common_mlx5_public-y) $(nvmeib_common_siw_public-y)

# Do not generate ftrace section
ccflags-remove-y += -pg

# Include trace.mk
include $(M)/scripts/trace.mk
