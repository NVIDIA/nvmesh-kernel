# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#
# OFED vs inbox RDMA paths, include dirs, and IB core module setup.
# Included from top-level Makefile after NVMESH_SRC_DIR, nconfig_* defines, KERN_VER, KSRC, kconfig.
# Requires: NVMESH_SRC_DIR, nconfig_set_val (for IB core module macro bodies at eval time).

include $(NVMESH_SRC_DIR)/scripts/core_modules.mk

ifeq ($(KERN_VER_NO_OFED),)
    # Check for exact match first
    ifneq ($(wildcard $(NVMESH_SRC_DIR)/kernels/$(KERN_VER)),)
        KERN_VER_NO_OFED=$(KERN_VER)
    else
        KERN_VER_NO_OFED = $(shell echo $(KERN_VER) | cut -f1,2,3 -d.)
        ifeq ($(wildcard $(NVMESH_SRC_DIR)/kernels/$(KERN_VER_NO_OFED)),)
            KERN_VER_NO_OFED = $(shell echo $(KERN_VER) | cut -f1,2 -d.)
        endif
    endif
endif


INC_DIR =
INC_DIR2 =

OFED_INFO =
OFED_VER_TYPE =
OFED_VER_MAJ =
OFED_VER_MIN =
OFED_VER_POINT_MAJ =
OFED_SRC_DIR =

INBOX_OFED_VER_STRING := none

ifeq ($(OFED_VER_TYPE),none)
    OFED_VER_STRING := $(INBOX_OFED_VER_STRING)
else
    ifneq ($(OFED_VER_TYPE),)
        OFED_VER_STRING = $(OFED_VER_TYPE)
        OFED_SRC_DIR = $(_OFED_SRC_DIR)
    else
        # Try and locate ofed_info
        ifeq ($(OFED_INFO),)
            OFED_INFO := $(shell which ofed_info 2>/dev/null)
        endif
        ifeq ($(OFED_INFO),)
            # OFED not found. Using INBOX Driver
            $(info ofed_info not found. Building against INBOX Driver)
            OFED_VER_TYPE := none
            OFED_VER_STRING := $(INBOX_OFED_VER_STRING)
        else
            OFED_VER_STRING := $(shell $(OFED_INFO) -s 2> /dev/null | grep -Eo "^[^: ]+")
            OFED_VER_TYPE := $(OFED_VER_STRING)
        endif
    endif
    ifneq ($(OFED_VER_STRING), $(INBOX_OFED_VER_STRING))
        OFED_FULL_VER := $(shell echo $(OFED_VER_STRING) | grep -Eo "[0-9.]+[0-9.-]+")
        OFED_VER := $(shell echo $(OFED_VER_STRING) | grep -Eo "[0-9.]+" | head -1)
        OFED_VER_MAJ := $(shell echo $(OFED_VER) | cut -d. -f1)
        OFED_VER_MIN := $(shell echo $(OFED_VER) | cut -d. -f2)
        OFED_VER_POINT := $(shell echo $(OFED_VER_STRING) | grep -Eo "[0-9.]+" | tail -1)
        OFED_VER_POINT_MAJ := $(shell echo $(OFED_VER_POINT) | cut -d. -f1)
    endif
endif

ifneq (,$(findstring MLNX_OFED_LINUX, $(OFED_VER_TYPE)))
    OFED_WE_R = yes
else
    ifeq ($(OFED_VER_TYPE), $(filter MLNX_OFED_LINUX% OFED-internal%,$(OFED_VER_TYPE)))
        OFED_WE_R = yes
    else
        OFED_WE_R = no
    endif
endif

# $(warning We are OFED, $(OFED_VER_TYPE))
#ifeq ($(OFED_VER_TYPE), $(filter $(OFED_VER_TYPE),MLNX_OFED_LINUX OFED-internal))
ifeq ($(OFED_WE_R), yes)
    # Mellanox OFED
    ifeq ($(OFED_SRC_DIR),)
        # OFED_SRC_DIR not defined - Check for DKMS
        OFED_DKMS_VERS := $(shell ofed_info -l | grep mlnx-ofed-kernel-dkms | awk '{print $$3;}')
        OFED_DKMS_VERS += $(shell ls /var/lib/dkms/mlnx-of*kernel/)
        OFED_DKMS_VER := $(firstword $(OFED_DKMS_VERS))
        ifneq ($(OFED_DKMS_VER),)
            OFED_DKMS_VER_MAJ_MIN := $(shell echo $(OFED_DKMS_VER) | cut -d. -f1,2)
            OFED_DKMS_VER_MAJ_MIN_POINT := $(shell echo $(OFED_DKMS_VER) | grep -Eo '[0-9]+.[0-9]+[.-]OFED[.-][0-9;.]+')
            # Check for all possibile dkms source dirs
            DKMS_SRC_DIRS := $(wildcard /var/lib/dkms/mlnx-of*kernel/$(OFED_DKMS_VER_MAJ_MIN_POINT)/source)
            DKMS_SRC_DIRS += $(wildcard /usr/src/mlnx-of*kernel-$(OFED_DKMS_VER_MAJ_MIN_POINT))
            DKMS_SRC_DIRS += $(wildcard /var/lib/dkms/mlnx-of*kernel/$(OFED_DKMS_VER_MAJ_MIN)/source)
            DKMS_SRC_DIRS += $(wildcard /usr/src/mlnx-of*kernel-$(OFED_DKMS_VER_MAJ_MIN))

            OFED_SRC_DIR := $(firstword $(DKMS_SRC_DIRS))
        endif
    endif
    ifeq ($(OFED_SRC_DIR),)
        # No DKMS, Check for /usr/src/mlnx-of[ed,a]-kernel-X.X
        OFED_SRC_DIRS := $(wildcard /usr/src/mlnx-of*kernel-$(OFED_VER))
        OFED_SRC_DIRS += $(wildcard /usr/src/mlnx-of*kernel-$(OFED_VER_MAJ).$(OFED_VER_MIN))
        OFED_SRC_DIRS += $(wildcard /usr/src/mlnx-of*kernel-$(OFED_FULL_VER))
        OFED_SRC_DIRS += $(wildcard /usr/src/mlnx-of*kernel-$(OFED_VER_MAJ).$(OFED_VER_MIN).$(OFED_VER_POINT_MAJ))
        OFED_SRC_DIR := $(firstword $(OFED_SRC_DIRS))
    endif
    ifeq ($(OFED_SRC_DIR),)
        ifeq ($(COMPILE_COMMON),yes)
            $(error OFED Source Dir not found provide with OFED_SRC_DIR=/path/to/mlnx-ofed_kernel/$(OFED_VER))
        endif
    endif
    $(info Using OFED_SRC_DIR=$(OFED_SRC_DIR))
    ifeq ($(OFA_KERNEL),)
        # Check for all possible ofa_kernel dirs
        OFA_KERNEL_DIR := $(wildcard /usr/src/ofa_kernel/$(KERN_ARCH)/$(KERN_VER))
        OFA_KERNEL_DIR += $(wildcard /usr/src/ofa_kernel/$(KERN_VER))
        OFA_KERNEL_DIR += $(wildcard /usr/src/ofa_kernel/default)
        OFA_KERNEL = $(firstword $(OFA_KERNEL_DIR))
    endif
    ifeq ($(OFA_KERNEL),)
        ifeq ($(COMPILE_COMMON),yes)
            $(error OFA Kernel Dir not found, provide with OFA_KERNEL=/path/to/ofa_kernel/default)
        endif
    endif
    $(info Using OFA_KERNEL=$(OFA_KERNEL))

    OFED_SYMVERS = $(wildcard $(OFA_KERNEL)/Module.symvers)
    INC_DIR += -I$(OFA_KERNEL)/include -I$(OFA_KERNEL)/include/uapi -I$(OFED_SRC_DIR)/drivers

    INFO_OFED := Mellanox OFED $(OFED_VER) in $(OFED_SRC_DIR). Symbols from $(OFED_SYMVERS)

    cflags += -DMLNX_OFED -DMLNX_OFED_$(OFED_VER_MAJ)_$(OFED_VER_MIN)
    cflags += -DOFED_VER_MAJ=$(OFED_VER_MAJ) -DOFED_VER_MIN=$(OFED_VER_MIN) -DOFED_VER_POINT_MAJ=$(OFED_VER_POINT_MAJ)

    export OFED_VER_MAJ
    export OFED_VER_MIN

    ifneq (,$(findstring $(OFED_VER_MAJ), 3 4 5))
        # include local dirs mlnx_ofed_X.X
        INC_DIR += -I$(NVMESH_SRC_DIR)/mlnx_ofed_$(OFED_VER)/include -I$(NVMESH_SRC_DIR)/mlnx_ofed_$(OFED_VER)/include/linux
    endif
    # KS_HAS_VIRT_DMA_SUPPORT, HAS_IB_QUERY_GID, __ib_alloc_pd, mlx5_ib.h, etc.: scripts/compute_backports.sh ($INC_RDMA / $INC_RDMA_DRV)

    # KS_HAS_KREF_READ: see scripts/compute_backports.sh (probed on KSRC1 only)

    # IB_HAS_CMA_PRIV_H: scripts/compute_backports.sh (file_exists_define on INC_RDMA_DRV)
    ifeq ($(COMPILE_COMMON),yes)
        $(eval $(call check_ofed_ib_core_modules))
        $(info obj-m $(obj-m))
    endif
else
    ifeq ($(OFED_VER_TYPE), OFED)
        # OFA OFED
        ifeq ($(OFED_SRC_DIR),)
            # OFED_SRC_DIR not defined - Check for /usr/src/compat-rdma-X.XX
            OFED_SRC_DIR := $(wildcard /usr/src/compat-rdma-$(OFED_VER))
            OFA_KERNEL := $(wildcard /usr/src/compat-rdma)
            OFED_SYMVERS = $(OFA_KERNEL)/Module.symvers
            ifeq ($(OFED_SRC_DIR),)
                # Not there
                ifeq ($(COMPILE_COMMON),yes)
                    $(error OFED Source Dir not found provide with OFED_SRC_DIR=/path/to/compat-rdma-$(OFED_VER))
                endif
            endif
            ifeq ($(OFA_KERNEL),)
                # Not there
                ifeq ($(COMPILE_COMMON),yes)
                    $(error OFA Kernel Source Dir not found provide with OFA_KERNEL=/path/to/compat-rdma)
                endif
            endif
        endif

        INFO_OFED := OFA OFED $(OFED_VER) in $(OFED_SRC_DIR). Symbols from $(OFED_SYMVERS)

        cflags += -DCOMPAT_RDMA -DCOMPAT_RDMA_$(OFED_VER_MAJ)_$(OFED_VER_MIN) -DCONFIG_COMPAT_IS_KTHREAD
        cflags += -DOFED_VER_MAJ=$(OFED_VER_MAJ) -DOFED_VER_MIN=$(OFED_VER_MIN)
        INC_DIR += -I$(OFED_SRC_DIR)/drivers -I$(OFA_KERNEL)/include -I$(OFA_KERNEL)/include/linux
        ifeq ($(COMPILE_COMMON),yes)
            $(eval $(call check_ofed_ib_core_modules))
            $(info obj-m $(obj-m))
        endif
    else
        ifeq ($(OFED_VER_TYPE),none)
            # INBOX Driver - Compile against Kernel Source
            KERN_FILES_PATH := $(NVMESH_SRC_DIR)/kernels/$(KERN_VER_NO_OFED)
            INFO_OFED := INBOX Driver. Building against Kernel $(KERN_VER) $(KERN_FILES_PATH)

            DIR := $(wildcard $(KERN_FILES_PATH))
            ifeq ($(DIR),)
                ifeq ($(COMPILE_COMMON),yes)
                    $(error $(KERN_FILES_PATH) not found. Kernel not supported)
                endif
            endif

            INC_DIR += -I$(KERN_FILES_PATH)/include -I $(KERN_FILES_PATH)/drivers
            cflags += -DNO_OFED -DKS_IB_SRQ_TYPE=0

            # KS_MLX5, ib_verbs.h, IB_HAS_CMA_PRIV_H: scripts/compute_backports.sh ($INC_RDMA / $INC_RDMA_DRV)

            ifeq ($(COMPILE_COMMON),yes)
                ifneq ($(wildcard $(KERN_FILES_PATH)/drivers/infiniband/core/Makefile),)
                    # If kernel symvers can be found, check the patched modules have the same symbols
                    # RHEL kernels store symvers in /boot/symvers-<kernel-version> or /boot/symvers-<kernel-version>.gz
                    # Ubuntu kernels store symvers in /usr/src/linux-headers-<kernel-version>/Module.symvers
                    KERN_SYMVERS = $(firstword \
                        $(wildcard /boot/symvers-$(KERN_VER_NO_OFED.gz)) \
                        $(wildcard /boot/symvers-$(KERN_VER_NO_OFED)) \
                        $(wildcard /usr/src/linux-headers-$(KERN_VER_NO_OFED)/Module.symvers))

                    $(eval $(call setup_kernel_ib_core_modules,kernels/$(KERN_VER_NO_OFED)/drivers/infiniband/core/Makefile,Kernel $(KERN_VER),$(KERN_SYMVERS)))
                else
                    INFO_CORE_MOD = NOT Building IB Core Modules for $(KERN_VER)
                endif
            endif
        else
            # Unknown OFED
            $(error Unknown OFED $(OFED_VER_STRING))
        endif
    endif
endif

# ofed_symbol_version: optional override for Module.symvers path
ifneq ($(OFED_SYM_VER),)
    OFED_SYMVERS = $(OFED_SYM_VER)
    INFO_OFED := Override - Mellanox OFED $(OFED_VER) in $(OFED_SRC_DIR). Symbols from $(OFED_SYMVERS)
endif

export OFED_VER_TYPE

ifneq ($(wildcard $(OFA_KERNEL)/compat/config.h),)
    INCLUDES = -include $(OFA_KERNEL)/compat/config.h
endif
