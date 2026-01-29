# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# nvmeibc.ko Makefile

# Include common.mk
include $(M)/scripts/common.mk

ARCH := $(shell uname -m | sed -e s/i.86/x86/ \
                                  -e s/sun4u/sparc64/ \
                                  -e s/arm.*/arm/ -e s/sa110/arm/ \
                                  -e s/s390x/s390/ -e s/parisc64/parisc/ \
                                  -e s/ppc.*/powerpc/ -e s/mips.*/mips/ \
                                  -e s/sh[234].*/sh/ -e s/aarch64.*/arm64/ )

ifeq ($(ARCH),x86_64)
	DP_EC_GF_ASM := block/datapath_ec/nvmeibc_block_dp_ec_gf_asm.o
	OBJECT_FILES_NON_STANDARD_block/datapath_ec/nvmeibc_block_dp_ec_gf_asm.o := y
endif
ifeq ($(ARCH),arm64)
	# DP_EC_GF_ASM := block/datapath_ec/nvmeibc_block_dp_ec_gf_asm_arm.o
	DP_EC_GF_ASM := \
		block/datapath_ec/nvmeibc_block_dp_ec_gf_arm_neon4.o \
		block/datapath_ec/nvmeibc_block_dp_ec_gf_arm_recov_neon.o \
		block/datapath_ec/nvmeibc_block_dp_ec_gf_arm_um.o
	# Needed for neon/crc32 instructions:
	# Note: nvmeibc_block_dp_ec_gf.o doesn't use neon/crc32 instructions, but uses their
	# availability to define NVMEIBC_GF_ARM_USER_SPACE_IMPLEMENTATION to enable gf_funcs_arm
	CFLAGS_block/datapath_ec/nvmeibc_block_dp_ec_gf_arm_neon4.o += -march=armv8.1-a+crc+simd -D__ARM_FEATURE_LS64=1
	CFLAGS_block/datapath_ec/nvmeibc_block_dp_ec_gf_arm_recov_neon.o += -march=armv8.1-a+crc+simd  -D__ARM_FEATURE_LS64=1
	CFLAGS_block/datapath_ec/nvmeibc_block_dp_ec_gf_arm_um.o +=  -march=armv8.1-a+crc+simd -D__ARM_FEATURE_LS64=1
	CFLAGS_block/datapath_ec/nvmeibc_block_dp_ec_gf.o += -march=armv8.1-a+crc+simd -D__ARM_FEATURE_LS64=1
	CFLAGS_REMOVE_block/datapath_ec/nvmeibc_block_dp_ec_gf_arm_neon4.o += -mgeneral-regs-only
	CFLAGS_REMOVE_block/datapath_ec/nvmeibc_block_dp_ec_gf_arm_recov_neon.o += -mgeneral-regs-only
	CFLAGS_REMOVE_block/datapath_ec/nvmeibc_block_dp_ec_gf_arm_um.o += -mgeneral-regs-only
	CFLAGS_REMOVE_block/datapath_ec/nvmeibc_block_dp_ec_gf.o += -mgeneral-regs-only

endif

obj-m += nvmeibc.o
nvmeibc-y := nvmeibc_main.o nvmeibc_volume.o nvmeibc_block.o \
	nvmeibc_cc_api.o nvmeibc_nvme.o nvmeibc_targets.o nvmeibc_memmgr_metrics.o \
	nvmeibc_channel.o \
	nvmeibc_admin_channel.o nvmeibc_ib_admin_channel.o \
	nvmeibc_ib_net.o nvmeibc_ib_net_admin.o \
	nvmeibc_disk.o nvmeibc_disk_locks.o \
	nvmeibc_disk_local_dma_pools.o \
	nvmeibc_locks_channel.o nvmeibc_toma.o \
	nvmeibc_ib_nordda_channel.o nvmeibc_ib_net_nordda.o nvmeibc_jam.o \
	nvmeibc_pausable.o \
	nvmeibc_capabilities.o \
	nvmeibc_disk_gen_cmds.o \
	nvmeibc_error_tags.o \
	nvmeibc_memmgr_metrics.o nvmeibc_common.o \
	module/instance/nvmeibc_cinst.o \
	block/nvmeibc_topology.o \
	block/nvmeibc_block_api_os.o \
	block/nvmeibc_block_api_conf.o \
	block/nvmeibc_block_api_nvmeshioctl.o \
	block/controlpath/nvmeibc_b_cp_lost_srv_resources.o \
	block/controlpath/nvmeibc_b_cp_cpu_masks.o \
	block/controlpath/nvmeibc_b_cp_send_toma_msg.o \
	block/controlpath/nvmeibc_b_cp_topo_on_free.o \
	block/controlpath/nvmeibc_b_cp_topo_seg_on_active.o \
	block/controlpath/nvmeibc_b_cp_trs_hash.o \
	block/datapath_ec/nvmeibc_block_dp_ec.o \
	block/datapath_ec/nvmeibc_block_dp_ec_gf.o \
	$(DP_EC_GF_ASM) \
	block/datapath_ec/nvmeibc_block_dp_ec_reed_solomon.o \
	block/datapath_ec/nvmeibc_block_dp_ec_gf_praid.o \
	block/datapath_ec/nvmeibc_block_dp_ec_journal.o \
	block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_cold.o \
	block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_cold_sync.o \
	block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_hot.o \
	block/datapath_ec/recov/nvmeibc_block_dp_ec_sync_txid_wraparound.o \
	block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_maintenance.o \
	block/datapath_ec/recov/nvmeibc_block_dp_ec_recovery_common.o \
	block/datapath_mirror/nvmeibc_block_dp_mirror.o \
	block/datapath_mirror/nvmeibc_block_dp_mirror_trim.o \
	block/datapath_mirror/nvmeibc_block_dp_mirror_sync.o \
	block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.o \
	block/datapath_utils_generic/nvmeibc_block_dp_buffers.o \
	block/datapath_utils_generic/nvmeibc_block_dp_cmd_lock_link.o \
	block/datapath_utils_generic/nvmeibc_block_dp_io_resubmitter.o \
	block/datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.o \
	block/datapath_utils_generic/operation/nvmeibc_block_dp_operation_locks_transfer.o \
	block/datapath_utils_generic/nvmeibc_block_dp_io_perm_alert.o \
	block/datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.o \
	block/datapath_utils_generic/nvmeibc_block_dp_common.o \
	block/datapath_utils_generic/nvmeibc_block_dp_cpu_masks.o \
	block/datapath_utils_generic/nvmeibc_block_dp_operation.o \
	block/datapath_utils_generic/nvmeibc_block_dp_lock_server.o \
	block/datapath_utils_generic/nvmeibc_block_dp_dbits.o \
	block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.o \
	block/datapath_utils_generic/nvmeibc_block_dp_locks_scheme.o \
	block/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.o \
	block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.o \
	block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_log.o \
	block/datapath_utils_generic/nvmeibc_block_dp_block_md_tables.o \
	block/datapath_utils_generic/nvmeibc_block_dp_block_md.o \
	block/datapath_utils_generic/profiling/nvmeibc_block_dp_profiling_generic.o \
	management_utils_common/nvmeibc_management_capi_parse_conf.o \
	management_utils_common/nvmeibc_management_volume_conf_checks.o \
	block/recovery/nvmeibc_raid_recov_itr.o \
	block/recovery/nvmeibc_raid_recovery.o \
	block/recovery/nvmeibc_block_dp_sync_common.o \
	block/recovery/nvmeibc_decentralized_unreg.o \
	block/recovery/nvmeibc_block_dp_sync_api_manager_flow_cntrs.o \
	block/recovery/nvmeibc_block_dp_sync_no_write_hole.o \
	block/derived_apis/nvmeibc_block_api_d_carrier.o \
	../toma/clnt/nvmeibt_client_protocol.o \
	vex/nvmeibc_vex_shared.o \
	traces_ids.o

ifneq ($(filter $(CORE_UNITEST), true yes),)
  nvmeibc-y += ../core_unitest/corecomm_kernel.o
  nvmeibc-y += ../core_unitest/corecomm_netlink_rpc_srv.o
  nvmeibc-y += ../core_unitest/corecomm_injections.o
endif

ccflags-y += -DALLOW_SIMULATED_MD -DTRACE_INCLUDE_FILE=\"nvmeibc_trace.h\"
ldflags-y += -T $(M)/clnt/nvmeibc.lds

OBJECT_FILES_NON_STANDARD_nvmeibc_block_dp_ec_gf_asm.o := y
STACKTOOL_nvmeibc_block_dp_ec_gf_asm.o := y

# Compile gen_events and dictionary for binary tracing
TRACE_MODULE := nvmeibc
TRACE_MODULE_OBJ := $(filter-out traces_ids.o,$(nvmeibc-y))

# Do not generate ftrace section
ccflags-remove-y += -pg

# Include trace.mk
include $(M)/scripts/trace.mk

