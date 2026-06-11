# Set the SHELL variable to bash (fix ubuntu issues)

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

SHELL:=/bin/bash

# Uncomment the lines below to debug the grep utility functions below
GREP_DEBUG:=1
GREP_DEBUG_LOGFILE:=/tmp/backports_mk_$(strip $(shell date +%s)).log

ifeq ($(GREP_DEBUG),1)
    $(info Logging backports.mk to $(GREP_DEBUG_LOGFILE))
    $(shell touch $(GREP_DEBUG_LOGFILE))
endif

# $1 - Define
# $2 - General Expression for struct/func/etc.
# $3 - General Expression for member/param/rv - Empty to match all
# $4 - File Path
# $5 - Kernel/OFED Path
# $6 - Found define value - default 1
# $7 - Not-found define value - default 0
#
# [NVMESH-3210] #1: Support multiple files using foreach (needed for submit_bio (either fs.h or bio.h)
# [NVMESH-3210] #2: Added the sed erase command to remove comments (was causing issues with submit_bio)
# [NVMESH-3210: #3: When GREP_DEBUG enabled, log to GREP_DEBUG_LOGFILE
define grep_common =
-D$(strip $(1))=$(shell $(if $(filter 1,$(GREP_DEBUG)), echo $(strip $(1)) >> $(GREP_DEBUG_LOGFILE); exec 19>>$(GREP_DEBUG_LOGFILE); BASH_XTRACEFD=19; set -x; ,) \
	sed -e '/\/\*/,/\*\//{s:/\*.*\*/::g; t; :a; /\/\*/,/\*\//{s:/\*.*\*/::g; t; N; b a}; s:/\*.*\*/::g}' $(foreach file, $(strip $(4)), $(5)/$(file)) 2>/dev/null | grep -Poz $(2) | \
	grep -Pq "$(strip $(3))" 2>/dev/null && echo -n $(if $(6),$(6),1) || echo -n $(if $(7),$(7),0))
endef

# Grep for struct member variable
# $1 - Define
# $2 - struct name
# $3 - struct member - empty to check if struct exists
# $4 - File Path
# $5 - Kernel/OFED Path
# $6 - Found define value - default 1
# $7 - Not-found define value - default 0
grep_struct_member = $(call grep_common,$(1),'(?s)$(strip $(2))\s+\{.*?(?=\n\};)\n\};\n',$(3),$(4),$(5),$(6),$(7))

# Grep for function parameter variable
# $1 - Define
# $2 - function name
# $3 - function variable name - empty to check if function exists
# $4 - File Path
# $5 - Kernel/OFED Path
# $6 - Found define value - default 1
# $7 - Not-found define value - default 0
grep_func_var = $(call grep_common,$(1),'(?s)$(strip $(2))\s*\(.*?\);\n',$(3),$(4),$(5),$(6),$(7))

# Grep for function return value type
# $1 - Define
# $2 - function name
# $3 - function return type
# $4 - File Path
# $5 - Kernel/OFED Path
# $6 - Found define value - default 1
# $7 - Not-found define value - default 0
grep_func_rv = $(call grep_common,$(1),'(?s)[^\n]*$(strip $(2))\s*\(.*?\);\n',(?s)$(strip $(3))\s*$(strip $(2)),$(4),$(5),$(6),$(7))

# Grep for macro parameter
# $1 - Define
# $2 - macro name
# $3 - parameter name - empty to check if macro exists
# $4 - File Path
# $5 - Kernel/OFED Path
# $6 - Found define value - default 1
# $7 - Not-found define value - default 0
grep_macro_param = $(call grep_common,$(1),'(?s)\#define\s+$(strip $(2))\s*\(.*?\)',$(3),$(4),$(5),$(6),$(7))

# Grep for function pointer types
# ------------------------------------------------------------------
# CAUTION!
# This is a bit dodgy because it doesn't first match the outer scope
# (struct, function parameter, etc.). This means that we could match
# multiple function pointers if they have a common name.
# ------------------------------------------------------------------

# Grep for function pointer parameter variable
# $1 - Define
# $2 - function name
# $3 - function variable name
# $4 - File Path
# $5 - Kernel/OFED Path
# $6 - Found define value - default 1
# $7 - Not-found define value - default 0
grep_func_ptr_var = $(call grep_func_var,$(1),\(\*\s*$(strip $(2))\),$(3),$(4),$(5),$(6),$(7))

# Grep for function pointer return value type
# $1 - Define
# $2 - function name
# $3 - function return type
# $4 - File Path
# $5 - Kernel/OFED Path
# $6 - Found define value - default 1
# $7 - Not-found define value - default 0
grep_func_ptr_rv = $(call grep_func_rv,$(1),\(\*\s*$(strip $(2))\),$(3),$(4),$(5),$(6),$(7))

# Grep for typedef
# $1 - Define
# $2 - Type name
# $3 - File Path
# $4 - Kernel/OFED Path
# $5 - Found define value - default 1
# $6 - Not-found define value - default 0
grep_typedef = $(call grep_common,$(1),'typedef\s+.*?\s+$(strip $(2))',,$(3),$(4),$(5),$(6))

# As above, but only look in kernel path $5 = $KSRC1
grep_ksrc_struct_member = $(call grep_struct_member,$(1),$(2),$(3),$(4),$(KSRC1),$(5),$(6))
grep_ksrc_func_var = $(call grep_func_var,$(1),$(2),$(3),$(4),$(KSRC1),$(5),$(6))
grep_ksrc_func_ptr_var = $(call grep_func_ptr_var,$(1),$(2),$(3),$(4),$(KSRC1),$(5),$(6))
grep_ksrc_macro_param = $(call grep_macro_param,$(1),$(2),$(3),$(4),$(KSRC1),$(5),$(6))
grep_ksrc_typedef = $(call grep_typedef,$(1),$(2),$(3),$(KSRC1),$(4),$(5))
grep_ksrc_func_ptr_rv = $(call grep_func_ptr_rv,$(1),$(2),$(3),$(4),$(KSRC1),$(5),$(6))
grep_ksrc_func_rv = $(call grep_func_rv,$(1),$(2),$(3),$(4),$(KSRC1))

ifneq ($(OFED_SRC_DIR),)
  INC_RDMA=$(OFED_SRC_DIR)
  INC_RDMA_DRV=$(OFED_SRC_DIR)
  RDMA_SYMVERS=$(OFED_SYMVERS)
else
  INC_RDMA=$(KSRC1)
  INC_RDMA_DRV=$(KERN_FILES_PATH)
  RDMA_SYMVERS=$(KERN_SYMVERS)
endif

# As above, but look in RDMA path $5 = $(OFA_KERNEL) for OFED or $(KSRC1) for INBOX
grep_rdma_struct_member = $(call grep_struct_member,$(1),$(2),$(3),$(4),$(INC_RDMA),$(5),$(6))
grep_rdma_func_var = $(call grep_func_var,$(1),$(2),$(3),$(4),$(INC_RDMA),$(5),$(6))
grep_rdma_func_rv = $(call grep_func_rv,$(1),$(2),$(3),$(4),$(INC_RDMA))
grep_rdma_func_ptr_var = $(call grep_func_ptr_var,$(1),$(2),$(3),$(4),$(INC_RDMA),$(5),$(6))
grep_rdma_func_ptr_rv = $(call grep_func_ptr_rv,$(1),$(2),$(3),$(4),$(INC_RDMA))
grep_rdma_macro_param = $(call grep_macro_param,$(1),$(2),$(3),$(4),$(INC_RDMA),$(5),$(6))
grep_rdma_typedef = $(call grep_typedef,$(1),$(2),$(3),$(INC_RDMA),$(4),$(5))

# As above, but look in RDMA drivers path $5 = $(OFA_KERNEL) for OFED or $(KERN_FILES_PATH) for INBOX
grep_rdma_drv_struct_member = $(call grep_struct_member,$(1),$(2),$(3),$(4),$(INC_RDMA_DRV),$(5),$(6))
grep_rdma_drv_func_var = $(call grep_func_var,$(1),$(2),$(3),$(4),$(INC_RDMA_DRV),$(5),$(6))
grep_rdma_drv_func_rv = $(call grep_func_rv,$(1),$(2),$(3),$(4),$(INC_RDMA_DRV))
grep_rdma_drv_func_ptr_var = $(call grep_func_ptr_var,$(1),$(2),$(3),$(4),$(INC_RDMA_DRV),$(5),$(6))
grep_rdma_drv_func_ptr_rv = $(call grep_func_ptr_rv,$(1),$(2),$(3),$(4),$(INC_RDMA_DRV))
grep_rdma_drv_macro_param = $(call grep_macro_param,$(1),$(2),$(3),$(4),$(INC_RDMA_DRV),$(5),$(6))
grep_rdma_drv_typedef = $(call grep_typedef,$(1),$(2),$(3),$(INC_RDMA_DRV),$(4),$(5))

# Check if a symbol is exported in a Module.symvers file
# $1 - Define
# $2 - Symbol name
# $3 - Path to Module.symvers
# $4 - Found define value - default 1
# $5 - Not-found define value - default 0
define grep_symvers =
-D$(strip $(1))=$(shell $(if $(filter 1,$(GREP_DEBUG)), echo $(strip $(1)) >> $(GREP_DEBUG_LOGFILE); exec 19>>$(GREP_DEBUG_LOGFILE); BASH_XTRACEFD=19; set -x; ,) grep -qw "$(strip $(2))" "$(strip $(3))" 2>/dev/null && echo -n $(if $(4),$(4),1) || echo -n $(if $(5),$(5),0))
endef

# grep_kern_symvers: Check if a symbol is exported by the kernel Module.symvers
grep_kern_symvers = $(call grep_symvers,$(1),$(2),$(KERN_SYMVERS),$(3),$(4))

# grep_rdma_symvers: Check if a symbol is exported by the kernel Module.symvers (INBOX) or the OFED Module.symvers
grep_rdma_symvers = $(call grep_symvers,$(1),$(2),$(RDMA_SYMVERS),$(3),$(4))

ARCH := $(shell uname -m | sed -e s/i.86/x86/ \
                                  -e s/x86_64/x86/ \
                                  -e s/sun4u/sparc64/ \
                                  -e s/arm.*/arm/ -e s/sa110/arm/ \
                                  -e s/s390x/s390/ -e s/parisc64/parisc/ \
                                  -e s/ppc.*/powerpc/ -e s/mips.*/mips/ \
                                  -e s/sh[234].*/sh/ -e s/aarch64.*/arm64/ )

# Kernel 5.10

cflags += $(call grep_rdma_func_ptr_rv, \
	         KS_IB_DESTROY_CQ_INT_RETURN, \
		 destroy_cq, \
		 int, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_rv, \
	         KS_IB_DESTROY_SRQ_INT_RETURN, \
		 destroy_srq, \
		 int, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_rv, \
	         KS_IB_DEALLOC_PD_INT_RETURN, \
		 dealloc_pd, \
		 int, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_rv, \
	         KS_IB_DESTROY_SRQ_RETURN_VOID, \
		 ib_destroy_srq, \
		 void, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_ksrc_struct_member, \
	         KS_REQUEST_QUEUE_HAS_REQUEST_FN, \
		 struct request_queue, \
		 make_request_fn, \
		 include/linux/blkdev.h)

cflags += $(call grep_rdma_func_var, \
	         KS_IB_REGISTER_DEVICE_HAS_DMA_DEVICE, \
		 int ib_register_device, \
		 struct device \*, \
		 include/rdma/ib_verbs.h)

##################################################################
cflags += $(call grep_ksrc_struct_member, \
	         KS_MLX5_ACCESS_MODE_1_0, \
		 mlx5_ifc_mkc_bits, \
		 access_mode_1_0 , \
		 include/linux/mlx5/mlx5_ifc.h)

cflags += $(call grep_rdma_func_var, \
	         HAS_IB_QUERY_GID, \
		 int ib_query_gid, \
		 struct ib_device \*device, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_var, \
	         KS_IB_HAS_RDMA_GET_GID_ATTR, \
		 struct ib_gid_attr \*rdma_get_gid_attr, \
		 int index, \
		 include/rdma/ib_cache.h)

cflags += $(call grep_rdma_struct_member, \
	         KS_IB_RDMA_PORT_SPACE, \
		 enum rdma_port_space, \
		 , \
		 include/rdma/rdma_cm.h)

cflags += $(call grep_ksrc_struct_member, \
	         KS_TCP_SOCK_HAS_XMIT_SIZE_GOAL_SEGS, \
		 struct tcp_sock, \
		 xmit_size_goal_segs, \
		 include/linux/tcp.h)

cflags += $(call grep_ksrc_struct_member, \
	         KS_DEV_ARCHDATA_HAS_DMA_OPS, \
		 struct dev_archdata, \
		 dma_ops, \
		 arch/$(ARCH)/include/asm/device.h)

cflags += $(call grep_rdma_func_var, \
	         KS_IB_REGISTER_DEVICE_HAS_NAME, \
		 int ib_register_device, \
		 const char \*name, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_ksrc_func_var, \
	         KS_GET_USER_PAGES_HAS_TASK_STRUCT, \
		 long get_user_pages, \
		 struct task_struct \*tsk, \
		 include/linux/mm.h)

cflags += $(call grep_ksrc_func_var, \
	         KS_GET_USER_PAGES_REMOTE_HAS_TASK_STRUCT, \
		 long get_user_pages_remote, \
		 struct task_struct \*tsk, \
		 include/linux/mm.h)

cflags += $(call grep_ksrc_func_var, \
	         KS_GET_USER_PAGES_HAS_VMAS, \
		 long get_user_pages, \
		 struct vm_area_struct \*\*vmas, \
		 include/linux/mm.h)

cflags += $(call grep_ksrc_func_var, \
	         KS_GET_USER_PAGES_REMOTE_HAS_VMAS, \
		 long get_user_pages_remote, \
		 struct vm_area_struct \*\*vmas, \
		 include/linux/mm.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_REG_USER_MR_HAS_ATTR, \
		 reg_user_mr, \
		 struct ib_mr_init_attr, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_var, \
	         KS_IB_REGISTER_DEVICE_HAS_KOBJECT, \
		 int ib_register_device, \
		 struct kobject, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_QUERY_DEVICE_HAS_UDATA, \
		 query_device, \
		 struct ib_udata, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_CREATE_CQ_HAS_IB_CQ_INIT_ATTR, \
		 create_cq, \
		 const struct ib_cq_init_attr, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_CREATE_AH_HAS_UDATA, \
		 create_ah, \
		 struct ib_udata, \
		 include/rdma/ib_verbs.h)

# pre CentOS 7.7 (at least acccording to commit 21541ed4f3d
# 'SIW: Code fixes to support CentOS 7.7' although does NOT exist in 3.10.0-693
#int (*process_mad)(struct ib_device *device, int process_mad_flags,
#	u8 port_num, struct ib_wc *in_wc,
#	struct ib_grh *in_grh,
#	struct ib_mad *in_mad, struct ib_mad *out_mad);

# CentOS 7.7 (till 4.18.0-193 i.e. RH 8.2-2004)
#int (*process_mad)(struct ib_device *device, int process_mad_flags,
#	 u8 port_num, const struct ib_wc *in_wc,
#	 const struct ib_grh *in_grh,
#	 const struct ib_mad_hdr *in_mad, size_t in_mad_size,
#	 struct ib_mad_hdr *out_mad, size_t *out_mad_size,
#	 u16 *out_mad_pkey_index);

# 4.18.0-240
#int (*process_mad)(struct ib_device *device, int process_mad_flags,
#	 u8 port_num, const struct ib_wc *in_wc,
#	 const struct ib_grh *in_grh,
#	 const struct ib_mad *in_mad, struct ib_mad *out_mad,
#	 size_t *out_mad_size, u16 *out_mad_pkey_index);

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_PROCESS_MAD_HAS_OUT_MAD_PKEY_INDEX, \
		 process_mad, \
		 u16 \*out_mad_pkey_index, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_PROCESS_MAD_HAS_IB_MAD_HDR, \
		 process_mad, \
		 struct ib_mad_hdr, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_POST_SRQ_RECV_HAS_CONST, \
		 post_srq_recv, \
		 const struct ib_recv_wr, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_POST_SEND_HAS_CONST, \
		 post_send, \
		 const struct ib_send_wr, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_ATTR_HAS_MAX_SEND_SGE, \
		 struct ib_device_attr, \
		 max_send_sge, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_HAS_OPS, \
		 struct ib_device, \
		 ib_device_ops, \
		 include/rdma/ib_verbs.h)

ifeq ($(shell echo $(cflags) | grep "KS_IB_DEVICE_HAS_OPS=1"),)
cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_ATTR_HAS_GET_PORT_IMMUTABLE, \
		 struct ib_device, \
		 get_port_immutable, \
		 include/rdma/ib_verbs.h)
else
cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_ATTR_HAS_GET_PORT_IMMUTABLE, \
		 struct ib_device_ops, \
		 get_port_immutable, \
		 include/rdma/ib_verbs.h)
endif

cflags += $(call grep_ksrc_struct_member, \
	         KS_DMA_MAP_OPS_HAS_DMA_ATTR, \
		 struct dma_map_ops, \
		 struct dma_attrs, \
		 include/linux/dma-mapping.h)

cflags += $(call grep_ksrc_func_var, \
	         KS_HAS_SMP_STORE_MB, \
		 define smp_store_mb, \
		 value, \
		 include/asm-generic/barrier.h)

# ---------------------------------------------------------------------------- #
# RH8
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_var, \
	         KS_SOCK_OPT_GETNAME_HAS_UADDR_LEN, \
		 inet_getname, \
		 uaddr_len, \
		 include/net/inet_common.h)

cflags += $(call grep_ksrc_struct_member, \
	         KS_THREAD_INFO_HAS_CPU, \
		 struct thread_info, \
		 cpu, \
		 arch/$(ARCH)/include/asm/thread_info.h)

cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_HAS_DEVICE_OPS, \
		 struct ib_device_ops, \
		 ops, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_ksrc_struct_member, \
	         KS_DEVICE_HAS_DEVICE_RH, \
		 struct device, \
		 device_rh, \
		 include/linux/device.h)

cflags += $(call grep_rdma_struct_member, \
		KS_IW_CM_HAS_IFNAME, \
		struct iw_cm_verbs, \
		ifname, \
		include/rdma/iw_cm.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_CREATE_AH_HAS_FLAGS, \
		 create_ah, \
		 u32, \
		 include/rdma/ib_verbs.h)

ifneq (,$(wildcard $(KSRC1)/include/linux/sched/mm.h))
cflags += -DLINUX_SCHED_MM=1
else
cflags += -DLINUX_SCHED_MM=0
endif

# ---------------------------------------------------------------------------- #
# Kernel 4.18.0-[147, 193] (CentOS 8.1.1911, 8.2.2004)
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_func_ptr_var, \
	         IB_DEREG_MR_HAS_UDATA, \
		 dereg_mr, \
		 struct ib_udata, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_OPS_HAS_MODULE_OWNER, \
		 struct ib_device_ops, \
		 owner, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_var, \
	         KS_IB_MLX5_WRITE64_HAS_DB_LOCK, \
		 static inline void mlx5_write64, \
		 spinlock_t \*doorbell_lock, \
		 include/linux/mlx5/doorbell.h)

cflags += $(call grep_common, \
	        KS_HAS___LLIST_ADD_BATCH, \
	        __llist_add_batch, \
	        , \
	        include/linux/llist.h, \
	        $(KSRC1))

cflags += $(call grep_common, \
	        KS_HAS___LLIST_ADD, \
	        __llist_add, \
	        , \
	        include/linux/llist.h, \
	        $(KSRC1))

cflags += $(call grep_common, \
	        KS_HAS___LLIST_DEL_ALL, \
	        __llist_del_all, \
	        , \
	        include/linux/llist.h, \
	        $(KSRC1))

# ---------------------------------------------------------------------------- #
# Kernel 4.20
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_struct_member, \
	         KS_MLX5_IB_FBC_HAS_FRAG_BUF, \
		 mlx5_frag_buf_ctrl, \
		 frag_buf\;, \
		 include/linux/mlx5/driver.h)
# ---------------------------------------------------------------------------- #
# OFED 5.1
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_func_var, \
	         KS_RDMA_REJECT_HAS_REASON, \
		 int rdma_reject, \
		 u8 reason, \
		 include/rdma/rdma_cm.h)

# ---------------------------------------------------------------------------- #
# RH8.3
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_drv_struct_member, \
	         IB_MLX5_MR_HAS_LIVE, \
		 mlx5_ib_mr, \
		 live, \
		 drivers/infiniband/hw/mlx5/mlx5_ib.h)

# ---------------------------------------------------------------------------- #
# RH8.4
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_drv_struct_member, \
	         IB_MLX5_MR_HAS_DEV, \
		 mlx5_ib_mr, \
		 dev, \
		 drivers/infiniband/hw/mlx5/mlx5_ib.h)

cflags += $(call grep_rdma_drv_struct_member, \
	         IB_MLX5_MR_HAS_NPAGES, \
		 mlx5_ib_mr, \
		 npages, \
		 drivers/infiniband/hw/mlx5/mlx5_ib.h)

cflags += $(call grep_rdma_drv_struct_member, \
	         IB_MLX5_QP_HAS_WQ_SIG, \
		 mlx5_ib_qp, \
		 wq_sig, \
		 drivers/infiniband/hw/mlx5/mlx5_ib.h)

# see also 'KS_PROCESS_MAD_' above for changes since 4.18.0-240

# ---------------------------------------------------------------------------- #
# Kernel 5.4.0-[1031, 1035] (Ubuntu 18.04 @ Azure)
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_func_var, \
	         KS_IB_REGISTER_DEVICE_HAS_IB_DEVICE, \
		 int ib_register_device, \
		 struct ib_device, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_HAS_OWNER, \
		 struct ib_device, \
		 owner, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_HAS_IWCM, \
		 struct ib_device, \
		 iwcm, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_DEVICE_ALLOC_USES_UCONTEXT, \
		 alloc_ucontext, \
		 struct ib_ucontext, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_DESTROY_QP_HAS_UDATA, \
		 destroy_qp, \
		 struct ib_udata, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_DEVICE_PD_USES_UCONTEXT, \
		 alloc_pd, \
		 struct ib_ucontext, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_CREATE_CQ_HAS_IB_DEVICE, \
		 create_cq, \
		 struct ib_device, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_CREATE_CQ_HAS_IB_UCONTEXT, \
		 create_cq, \
		 struct ib_ucontext, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_DESTROY_CQ_HAS_IB_UDATA, \
		 destroy_cq, \
		 struct ib_udata, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_ALLOC_MR_HAS_IB_UDATA, \
		 alloc_mr, \
		 struct ib_udata, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_DEREG_MR_HAS_IB_UDATA, \
		 dereg_mr, \
		 struct ib_udata, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_CREATE_SQR_HAS_IB_PD, \
		 create_srq, \
		 struct ib_pd, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_DESTROY_SQR_HAS_IB_UDATA, \
		 destroy_srq, \
		 struct ib_udata, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_var, \
	         KS_IB_SET_NETDEV_HAS_IB_DEVICE, \
		 int ib_device_set_netdev, \
		 struct ib_device, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_OPS_HAS_SIZES, \
		 struct ib_device_ops, \
		 DECLARE_RDMA_OBJ_SIZE, \
		 include/rdma/ib_verbs.h)


# ---------------------------------------------------------------------------- #
# OFED 5.2 (Kernel 5.10)
# ---------------------------------------------------------------------------- #
# return type of 'add' func-ptr in 'struct ib_client': void -> int
# CAUTION! Matches any function pointer called add in the file. See above
cflags += $(call grep_rdma_func_ptr_rv, \
	         KS_IB_CLIENT_ADD_RV_IS_INT, \
		 add, \
		 int, \
		 include/rdma/ib_verbs.h)
		 
 cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_CLIENT_REMOVE_HAS_CLIENT_DATA, \
		 remove, \
		 client_data, \
		 include/rdma/ib_verbs.h)

# fmr support removed
cflags += $(call grep_rdma_struct_member, \
	         KS_IB_VERBS_SUPPORTS_FMR, \
		 struct ib_device_ops, \
		 alloc_fmr, \
		 include/rdma/ib_verbs.h)

# ---------------------------------------------------------------------------- #
# Remove the need for the RHEL_BKPORT_MLX5 switch
# ---------------------------------------------------------------------------- #
ifneq ($(wildcard $(INC_RDMA)/include/linux/mlx4/qp.h),)
cflags += $(call grep_rdma_struct_member, \
	         KS_MLX_FENCE_VLAN, \
		 struct mlx4_wqe_ctrl_seg, \
		 qpn_vlan, \
		 include/linux/mlx4/qp.h)
endif

# Set to 0 if struct mlx5_create_mkey_mbox_in exists
cflags += $(call grep_rdma_struct_member, \
	         KS_MLX5_IFC, \
		 struct mlx5_create_mkey_mbox_in, \
		 , \
		 include/linux/mlx5/device.h, \
		 0, \
		 1)
# ---------------------------------------------------------------------------- #
# Kernel 4.18.0-193
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_CREATE_AH_HAS_PD, \
		 create_ah, \
		 struct ib_pd, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_CREATE_AH_HAS_AH, \
		 create_ah, \
		 struct ib_ah, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_rv, \
	         KS_IB_DESTROY_AH_RETURNS_INT, \
		 destroy_ah, \
		 int, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_rv, \
	         KS_IB_DESTROY_AH_RETURNS_VOID, \
		 destroy_ah, \
		 void, \
		 include/rdma/ib_verbs.h)

# ---------------------------------------------------------------------------- #
# Kernel 4.18.0-305
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_CREATE_AH_HAS_AH_INIT_ATTR, \
		 create_ah, \
		 struct rdma_ah_init_attr, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_DESTROY_AH_HAS_FLAGS, \
		 destroy_ah, \
		 u32, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_struct_member, \
	         KS_IB_HAS_FMR, \
		 struct ib_fmr, \
		 , \
		 include/rdma/ib_verbs.h)

# ---------------------------------------------------------------------------- #
# Kernel 4.18.0-305.10.2.el8_4 (for RH8.4 and vanilla 5.8)
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_var, \
		KS_HAS_TCP_SOCK_SET_NODELAY, \
		tcp_sock_set_nodelay, \
		, \
		include/linux/tcp.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_TCP_SOCK_SET_QUICKACK, \
		tcp_sock_set_quickack, \
		, \
		include/linux/tcp.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_TCP_SETSOCKOPT, \
		tcp_setsockopt, \
		, \
		include/net/tcp.h)

cflags += $(call grep_ksrc_func_var, \
		KS_TCP_SETSOCKOPT_TAKES_SOCKPTR_T, \
		tcp_setsockopt, \
		sockptr_t, \
		include/net/tcp.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_TCP_GETSOCKOPT, \
		tcp_getsockopt, \
		, \
		include/net/tcp.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_SOCK_SETSOCKOPT, \
		sock_setsockopt, \
		, \
		include/net/sock.h)

cflags += $(call grep_ksrc_func_var, \
		KS_SOCK_SETSOCKOPT_TAKES_SOCKPTR_T, \
		sock_setsockopt, \
		sockptr_t, \
		include/net/sock.h)

# ---------------------------------------------------------------------------- #
# Used for SIW panic_remote_on_rx_err 4.14 and above
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_var, \
		KS_HAS_KERNEL_SENDMSG_LOCKED, \
		kernel_sendmsg_locked, \
		, \
		include/linux/net.h)

# ---------------------------------------------------------------------------- #
# Used for SIW IB_EVENT_GID_CHANGED 5.4 and above
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_HAS_EVENT_HADNLER_RWSEM, \
		 struct ib_device, \
		 event_handler_rwsem, \
		 include/rdma/ib_verbs.h)

# ---------------------------------------------------------------------------- #
# Used for nonexists ib_dma_alloc_coherent 5.11 and above
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_func_var, \
		KS_HAS_IB_DMA_ALLOC_COHERENT, \
		ib_dma_alloc_coherent, \
		, \
		include/rdma/ib_verbs.h)

# ---------------------------------------------------------------------------- #
# SIW - If INET_MATCH does not look at sdif, then listen binding using bound_dev_if fails loopback
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_macro_param, \
		KS_INET_MATCH_HAS_SDIF, \
		INET_MATCH, \
		__sdif, \
		include/net/inet_hashtables.h)

# ---------------------------------------------------------------------------- #
# SIW - Moved from Makefile
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_var, \
		KS_HAS_SOCK_SET_REUSEADDR, \
		sock_set_reuseaddr, \
		, \
		include/net/sock.h)

# ---------------------------------------------------------------------------- #
# Kernel 4.15
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_OPS_HAS_DRIVER_ID, \
		 struct ib_device_ops, \
		 driver_id, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_struct_member, \
	KS_IB_DEVICE_HAS_DRIVER_ID, \
	struct ib_device, \
	driver_id, \
	include/rdma/ib_verbs.h)

# ---------------------------------------------------------------------------- #
# 4.18.0-240.el8.x86_64
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_var, \
		KS_HAS_IDR_INIT, \
		idr_init, \
		, \
		include/linux/idr.h)

# ---------------------------------------------------------------------------- #
# 4.18.0-513.24.1.el8_9.x86_64
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_func_var, \
                 KS_IB_SA_PATH_REC_GET_CB_HAS_NUM_PRS, \
                 int ib_sa_path_rec_get, \
                 num_prs, \
                 include/rdma/ib_sa.h)
cflags += $(call grep_rdma_func_var, \
                 KS_IB_SA_PATH_REC_GET_CB_HAS_NUM_PRS_UINT, \
                 int ib_sa_path_rec_get, \
                 unsigned int num_prs, \
                 include/rdma/ib_sa.h)

# ---------------------------------------------------------------------------- #
# 5.12 Kernel
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_struct_member, \
		KS_HAS_BLOCK_DEVICE_STRUCT, \
		block_device, \
		, \
		include/linux/blk_types.h)

cflags += $(call grep_common, \
		KS_HAS_MODULE_MUTEX, \
		struct\s+mutex\s+module_mutex, \
		, \
		include/linux/module.h, \
		$(KSRC1), 1, 0)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_REVALIDATE_DISK_FN, \
		revalidate_disk, \
		, \
		include/linux/fs.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_SET_CAPACITY_AND_MODIFY_FN_GENDISK, \
		set_capacity_and_notify, \
		, \
		include/linux/genhd.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_DISK_PART_ITER, \
		disk_part_iter_init, \
		, \
		include/linux/genhd.h)

cflags += $(call grep_ksrc_struct_member, \
		KS_BIO_HAS_BI_BDEV_PTR, \
		struct bio,   \
		struct block_device.*\*, \
		include/linux/blk_types.h)

cflags += $(call grep_ksrc_struct_member, \
		KS_BIO_HAS_BI_GENDISK_PTR, \
		struct bio,   \
		struct gendisk.*\*, \
		include/linux/blk_types.h)

cflags += $(call grep_ksrc_struct_member, \
		KS_BIO_HAS_BI_OPF, \
		struct bio,   \
		bi_opf, \
		include/linux/blk_types.h)

cflags += $(call grep_ksrc_struct_member, \
		KS_BIO_HAS_BI_WRITE_HINT, \
		struct bio,   \
		bi_write_hint, \
		include/linux/blk_types.h)

# ---------------------------------------------------------------------------- #
# 5.13 Kernel
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_var, \
		KS_LIST_SORT_CMP_FUNC_T, \
		list_sort, \
		list_cmp_func_t, \
		include/linux/list_sort.h)

cflags += $(call grep_rdma_func_var, \
		IB_VERBS_PORT_NUM_IS_U32,\
		ib_query_port, \
		u32\s+port_num, \
		include/rdma/ib_verbs.h)

cflags += $(call grep_ksrc_struct_member, \
		KS_BLOCK_DEV_OPS_HAS_REVALIDATE_DISK, \
		block_device_operations, \
		revalidate_disk, \
		include/linux/blkdev.h)

# ---------------------------------------------------------------------------- #
# 5.14 Kernel
# ---------------------------------------------------------------------------- #

# ---------------------------------------------------------------------------- #
# 5.15 Kernel
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_struct_member, \
		KS_BDI_PTR_IN_QUEUE, \
		request_queue,   \
		struct backing_dev_info.*\*, \
		include/linux/blkdev.h) \

cflags += $(call grep_ksrc_struct_member, \
		KS_BDI_IN_QUEUE, \
		request_queue,   \
		struct backing_dev_info, \
		include/linux/blkdev.h) \


cflags += $(call grep_rdma_func_ptr_rv, \
	         KS_IB_CREATE_QP_INT_RV, \
		 create_qp, \
		 int, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_ptr_rv, \
	         KS_IB_CREATE_QP_INT_RV, \
		 create_qp, \
		 int, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_OPS_HAS_QP_SIZE, \
		 struct ib_device_ops, \
		 DECLARE_RDMA_OBJ_SIZE\(ib_qp\);, \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_REVALIDATE_DISK_FN, \
		revalidate_disk, \
		, \
		include/linux/fs.h)

# ---------------------------------------------------------------------------- #
# SIW - 4.15.0-136-generic
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_struct_member, \
	         KS_IB_VERBS_HAS_DMA_MAPPING_OPS, \
		 struct ib_dma_mapping_ops, \
		 , \
		 include/rdma/ib_verbs.h)

cflags += $(call grep_ksrc_struct_member, \
	         KS_DMA_MAP_OPS_HAS_MAPPING_ERROR, \
		 struct dma_map_ops, \
		 mapping_error, \
		 include/linux/dma-mapping.h)

cflags += $(call grep_ksrc_struct_member, \
	         KS_DMA_MAP_OPS_HAS_SET_DMA_MASK, \
		 struct dma_map_ops, \
		 set_dma_mask, \
		 include/linux/dma-mapping.h)

cflags += $(call grep_ksrc_struct_member, \
	         KS_DMA_MAP_OPS_HAS_IS_PHYS, \
		 struct dma_map_ops, \
		 is_phys, \
		 include/linux/dma-mapping.h)

cflags += $(call grep_rdma_struct_member, \
	         KS_IB_DEVICE_HAS_DMA_OPS, \
		 struct ib_device_ops, \
		 ops, \
		 include/rdma/ib_verbs.h)

# ---------------------------------------------------------------------------- #
# MLNX_OFED_LINUX-5.7-1.0.0.0 / 4.18.0-477.10.1.el8_8.x86_64
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_func_var, \
	         KS_RDMA_MLX5_CORE_CREATE_MKEY_MKEY_U32, \
			 int mlx5_core_create_mkey, \
			 u32 \*mkey, \
			 include/linux/mlx5/driver.h)

cflags += $(call grep_rdma_drv_struct_member, \
	         KS_RDMA_MLX5_IB_MKEY_HAS_NDESCS, \
			 struct mlx5_ib_mkey, \
			 unsigned int ndescs, \
			 drivers/infiniband/hw/mlx5/mlx5_ib.h)

# ---------------------------------------------------------------------------- #
# Lustre 2.15.2 / 4.18.0-425.3.1.el8_lustre.x86_64
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_func_var, \
	         KS_RDMA_MLX5_MISSING_MLX5_BUF_OFFSET, \
			 mlx5_buf_offset, \
			 , \
			 include/linux/mlx5/driver.h, 0, 1)

#######################################################################################
# Compat for smp_call_function_single_async
#######################################################################################
cflags += $(call grep_ksrc_typedef, \
		KS_SMP_CALL_SINGLE_DATA_T, \
		call_single_data_t, \
		include/linux/smp.h)


# ---------------------------------------------------------------------------- #
# SIW - For CQ notification as tasklet
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_var, \
		KS_HAS_TASKLET_SETUP, \
		tasklet_setup, \
		, \
		include/linux/interrupt.h)

# ---------------------------------------------------------------------------- #
# Missing scatterlist funtionality in 3.10 kernels
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_var, \
		KS_HAS_SGL_ALLOC_ORDER, \
		sgl_alloc_order, \
		, \
		include/linux/scatterlist.h)

cflags += $(call grep_ksrc_struct_member, \
		KS_HAS_SG_DMA_PAGE_ITER, \
		sg_dma_page_iter, \
		, \
		include/linux/scatterlist.h)

# ---------------------------------------------------------------------------- #
# OFED 23.07
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_func_var, \
	         KS_RDMA_IB_CM_LISTEN_HAS_SERVICE_MASK, \
			 int ib_cm_listen, \
			 __be64 service_mask, \
			 include/rdma/ib_cm.h)


# KS_INET_MATCH_HAS_SDIF only cover the case for old INET_MATCH macro.. on newer kenrels it's now an inline function
cflags += $(call grep_ksrc_func_var, \
		KS_HAS_NEW_INET_MATCH_LOWER, \
		inet_match, \
		, \
		include/net/inet_hashtables.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_NEW_INET_MATCH_CAPS, \
		INET_MATCH, \
		, \
		include/net/inet_hashtables.h)


# ---------------------------------------------------------------------------- #
# Kernel 5.17
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_rv, \
		KS_SUBMIT_BIO_VOID_RV, \
		submit_bio, \
		void, \
		include/linux/bio.h include/linux/fs.h)

cflags += $(call grep_ksrc_func_rv, \
		KS_ADD_DISK_INT_RV, \
		add_disk, \
		int\s*__must_check, \
		include/linux/blkdev.h)

# ---------------------------------------------------------------------------- #
# Kernel 5.18
# ---------------------------------------------------------------------------- #

cflags += $(call grep_ksrc_func_var, \
	         KS_BIO_INIT_HAS_BDEV_N_OPF, \
		 bio_init, \
		 opf, \
		 include/linux/bio.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_SET_CAPACITY_AND_MODIFY_FN_BDEV, \
		set_capacity_and_notify, \
		, \
		include/linux/blkdev.h)

# ---------------------------------------------------------------------------- #
# Kernel 5.19
# ---------------------------------------------------------------------------- #

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_BLK_MQ_DESTROY_QUEUE, \
		blk_mq_destroy_queue, \
		, \
		include/linux/blk-mq.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_SET_FS, \
		set_fs, \
		, \
		include/asm-generic/uaccess.h)

cflags += $(call grep_ksrc_func_var, \
		KS_BIO_ALLOC_HAS_BLOCK_DEVICE,\
		bio_alloc,\
		struct block_device,\
		include/linux/bio.h)

cflags += $(call grep_ksrc_func_var, \
		KS_BLKDEV_ISSUE_DISCARD_HAS_FLAGS,\
		blkdev_issue_discard,\
		unsigned long flags,\
		include/linux/blkdev.h)

# return type of 'submit_bio' func
cflags += $(call grep_ksrc_func_rv,\
		KS_BLOCK_DEV_MAKE_REQUEST_VOID,\
		submit_bio,\
		void,\
		include/linux/bio.h include/linux/fs.h)

cflags += $(call grep_ksrc_func_rv,\
		KS_BLK_QC_T,\
		submit_bio,\
		blk_qc_t,\
		include/linux/bio.h include/linux/fs.h)

cflags += $(call grep_ksrc_func_var,\
		KS_HAS_PROFILE_EVENT_REGISTER,\
		profile_event_register,\
		,\
		include/linux/profile.h)

cflags += $(call grep_ksrc_func_var,\
		KS_PDE_DATA_IS_LOWER,\
		pde_data,\
		,\
		include/linux/proc_fs.h)

cflags += $(call grep_ksrc_func_var,\
		KS_HAS_BLKDEV_IOCTL,\
		blkdev_ioctl,\
		,\
		include/linux/fs.h)

cflags += $(call grep_rdma_func_var, \
	         KS_IB_REGISTER_DEVICE_HAS_DEVICE, \
		 int ib_register_device, \
		 struct device, \
		 include/rdma/ib_verbs.h)

# ---------------------------------------------------------------------------- #
# Kernel 6.5
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_var, KS_BLKDEV_GET_BY_PATH_HAS_HOLDERS, blkdev_get_by_path, blk_holder_ops, include/linux/blkdev.h)

# ---------------------------------------------------------------------------- #
# Kernel 6.8
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_var, \
		KS_HAS_VM_FLAGS_SET, \
		vm_flags_set, \
		, \
		include/linux/mm.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_BIO_SET_OP_ATTRS, \
		bio_set_op_attrs, \
		, \
		include/linux/blktypes.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_BDEV_OPEN_BY_PATH, \
		bdev_open_by_path, \
		, \
		include/linux/blkdev.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_BLK_CLEANUP_DISK, \
		blk_cleanup_disk, \
		, \
		include/linux/genhd.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_TCP_SENDPAGE, \
		tcp_sendpage, \
		, \
		include/net/tcp.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_SENDPAGE_OK, \
		sendpage_ok, \
		, \
		include/linux/net.h)

cflags += $(call grep_ksrc_func_var, KS_HAS_RDMA_FOR_EACH_PORT, rdma_for_each_port, , include/rdma/ib_verbs.h)
# ---------------------------------------------------------------------------- #
# Kernel 6.9
# ---------------------------------------------------------------------------- #

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_BDEV_FILE_OPEN_BY_PATH, \
		bdev_file_open_by_path, \
		, \
		include/linux/blkdev.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_QUEUE_LIMITS_START_UPDATE, \
		queue_limits_start_update, \
		, \
		include/linux/blkdev.h)

cflags += $(call grep_ksrc_macro_param, \
		KS_HAS_BLK_ALLOC_DISK, \
		blk_alloc_disk, \
		, \
		include/linux/blkdev.h include/linux/genhd.h)

cflags += $(call grep_ksrc_macro_param, \
		KS_BLK_ALLOC_DISK_2PARAMS, \
		blk_alloc_disk, \
		lim, \
		include/linux/blkdev.h include/linux/genhd.h)

# ---------------------------------------------------------------------------- #
# DOCA OFED 24.10
# ---------------------------------------------------------------------------- #

cflags += $(call grep_rdma_func_ptr_var, \
	         KS_IB_CREATE_CQ_HAS_ATTR_BUNDLE, \
		 create_cq, \
		 struct uverbs_attr_bundle, \
		 include/rdma/ib_verbs.h)

# ---------------------------------------------------------------------------- #
# [NVMESH-5532]
# ---------------------------------------------------------------------------- #

cflags += $(call grep_ksrc_func_var,\
		KS_HAS_SOCK_NOT_OWNED_BY_ME,\
		sock_not_owned_by_me,\
		,\
		include/net/sock.h)


cflags += $(call grep_ksrc_func_var, \
		KS_HAS_FIND_NTH_BIT, \
		find_nth_bit, \
		, \
		include/linux/find.h)

# ---------------------------------------------------------------------------- #
# Kernel 6.14
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_var, \
		KS_HAS_BDEV_PARTNO, \
		bdev_partno, \
		, \
		include/linux/blkdev.h)
		
# ---------------------------------------------------------------------------- #
# Used for keeper
# ---------------------------------------------------------------------------- #
cflags += $(call grep_rdma_struct_member, \
		KS_RDMA_HAS_RESTRACK, \
		struct rdma_restrack_entry, \
		, \
		include/rdma/restrack.h)

cflags += $(call grep_ksrc_struct_member, \
	         KS_HAS_PROC_FS, \
		 struct proc_ops, \
		 , \
		 include/linux/proc_fs.h)

cflags += $(call grep_rdma_func_var, \
	         HAS_IB_GET_DMA_MR, \
		 ib_get_dma_mr, \
		 , \
		 include/rdma/ib_verbs.h)
 
# ---------------------------------------------------------------------------- #
# Kernel 6.17
# ---------------------------------------------------------------------------- #
cflags += $(call grep_ksrc_func_var, \
		KS_HAS_DEL_TIMER_SYNC, \
		del_timer_sync, \
		, \
		include/linux/timer.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS___INIT_TIMER, \
		__init_timer, \
		, \
		include/linux/timer.h)

cflags += $(call grep_ksrc_func_var, \
		KS_CRC32C_USES_SIZE_T, \
		crc32c, \
		size_t, \
		include/linux/crc32.h)

cflags += $(call grep_ksrc_struct_member, \
		KS_HAS_SKB_CHECKSUM_OPS, \
		struct skb_checksum_ops, \
		, \
		include/linux/skbuff.h include/net/checksum.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS___CRC32C_LE_COMBINE, \
		__crc32c_le_combine, \
		, \
		include/linux/crc32.h)

cflags += $(call grep_ksrc_func_var, \
		KS_HAS_HRTIMER_INIT, \
		hrtimer_init, \
		, \
		include/linux/hrtimer.h)

cflags += $(call grep_rdma_func_ptr_var, \
		KS_IB_REG_USER_MR_HAS_DMAH, \
		reg_user_mr, \
		ib_dmah, \
		include/rdma/ib_verbs.h)

cflags += $(call grep_rdma_func_var, \
		KS_HAS_MLX5_GET_UARS_PAGE, \
		mlx5_get_uars_page, \
		, \
		include/linux/mlx5/driver.h)

cflags += $(call grep_kern_symvers, \
		KS_TCP_SETSOCKOPT_EXPORTED, \
		tcp_setsockopt)