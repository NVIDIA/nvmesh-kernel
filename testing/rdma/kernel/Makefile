# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# our kernel modules clnt for client and srv for server

ifndef IM_SERVER
    obj-m += cltib.o
    cltib-y := clt.o
    ifdef IM_CLIENT
        INFO_SERV_CLNT = Building Client ONLY
    endif
endif

ifndef IM_CLIENT
    obj-m += srvib.o
	srvib-y :=	srv.o

    ifndef IM_SERVER
        INFO_SERV_CLNT = Building BOTH Server and Client

        obj-m += cltib.o
		cltib-y := clt.o

    else
        INFO_SERV_CLNT = Building Server ONLY
    endif
endif

# the kernel sources
ifeq ($(KERN_VER),)
        KERN_VER := $(shell uname -r)
endif

KSRC := /lib/modules/$(KERN_VER)/build
KSRC1 := /lib/modules/$(KERN_VER)/source

ifeq ($(wildcard $(KSRC1)/include/linux/hashtable.h),)
        # hashtable.h does not exist
        cflags += -DKS_HASHTABLE=0
else
        cflags += -DKS_HASHTABLE=1
endif
ifeq ($(shell grep reinit_completion $(KSRC1)/include/linux/completion.h 2> /dev/null),)
        # reinit_completion is not defined
        cflags += -DKS_REINIT_COMPLETION=0
else
        cflags += -DKS_REINIT_COMPLETION=1
endif

# when we use the OFED package we must use OFED includes before that
# the default kernel includes otherwise we end up with ib_structures mismatch.
# in order to override the linux main makefile include search path we replace
# the default LINUXINCLUDE global variable.  To replace the LINUXINCLUDE
# we must define the following lines in addition to the LINUXINCLUDE in the
# compile command
autoconf_h=$(shell /bin/ls -1 $(KSRC)/include/*/autoconf.h 2> /dev/null | head -1)
kconfig_h=$(shell /bin/ls -1 $(KSRC)/include/*/kconfig.h 2> /dev/null | head -1)

ifeq ($(kconfig_h),)
kconfig_h=$(shell /bin/ls -1 $(KSRC1)/include/*/kconfig.h 2> /dev/null | head -1)
endif

ifneq ($(kconfig_h),)
KCONFIG_H = -include $(kconfig_h)
endif

ifeq ($(M),)
        NVMESH_SRC_DIR = $(shell pwd)
else
        NVMESH_SRC_DIR= $(M)
endif

INC_DIR =
INC_DIR2 =

OFED_INFO =
OFED_VER_TYPE =
OFED_VER_MAJ =
OFED_VER_MIN =
OFED_SRC_DIR =

# Try and locate ofed_info
ifeq ($(OFED_INFO),)
    OFED_INFO := $(shell which ofed_info 2>/dev/null)
    ifeq ($(OFED_INFO),)
        # OFED not found. Check if OFED_VER_TYPE set to NONE
        ifneq ($(OFED_VER_TYPE),none)
            $(info If OFED is installed, provide path with OFED_INFO=/path/to/ofed_info)
            $(info For building without OFED set OFED_VER_TYPE=none. Requires supported Kernel in $(NVMESH_SRC_DIR)/kernels)
            $(error ofed_info not found)
        endif
        OFED_VER_STRING := NONE
    else
        OFED_VER_STRING := $(shell $(OFED_INFO) -s 2> /dev/null | grep -Eo "^[^: ]+")
        OFED_VER_TYPE := $(shell echo $(OFED_VER_STRING) | grep -Eo "[A-Za-z_-]+[A-Za-z]+")
        OFED_VER := $(shell echo $(OFED_VER_STRING) | grep -Eo "[0-9.]+" | head -1)
        OFED_VER_MAJ := $(shell echo $(OFED_VER) | cut -d. -f1)
        OFED_VER_MIN := $(shell echo $(OFED_VER) | cut -d. -f2)
    endif
endif

ifeq ($(OFED_VER_TYPE), $(filter $(OFED_VER_TYPE),MLNX_OFED_LINUX OFED-internal))
    # Mellanox OFED
    ifeq ($(OFED_SRC_DIR),)
        # OFED_SRC_DIR not defined - Check for /usr/src/mlnx-of[ed,a]-kernel-X.X
        DIR := $(wildcard /usr/src/mlnx-of*kernel-$(OFED_VER))
        ifneq ($(DIR),)
            # Exists
            OFED_SRC_DIR = $(DIR)
            OFA_KERNEL = /usr/src/ofa_kernel/default
            OFED_SYMVERS = $(OFA_KERNEL)/Module.symvers
            INC_DIR += -I$(OFED_SRC_DIR)/drivers -I$(OFA_KERNEL)/include
        else
            # Not there - Check for /var/lib/dkms/mlnx-of[ed,a]-kernel-X.X
            DIR := $(wildcard /var/lib/dkms/mlnx-of*kernel/$(OFED_VER))
            ifneq ($(DIR))
                # Exists
                OFED_SRC_DIR = $(DIR)
                OFED_SYMVERS = $(DIR)/build/Module.symvers
                INC_DIR += -I$(OFED_SRC_DIR)/build/include -I$(OFED_SRC_DIR)/build/drivers
                INC_DIR += -I$(OFED_SRC_DIR)/build/include/linux
            else
                $(error OFED Source Dir not found provide with OFED_SRC_DIR=/path/to/mlnx-ofed_kernel/$(OFED_VER))
            endif
        endif
    endif

    INFO_OFED := Mellanox OFED $(OFED_VER) in $(OFED_SRC_DIR). Symbols from $(OFED_SYMVERS)

    cflags += -DMLNX_OFED -DMLNX_OFED_$(OFED_VER_MAJ)_$(OFED_VER_MIN)
    cflags += -DOFED_VER_MAJ=$(OFED_VER_MAJ) -DOFED_VER_MIN=$(OFED_VER_MIN)

    ifeq ($(OFED_VER_MAJ), 3)
        # Mellanox OFED 3.X
        cflags += -DCONFIG_COMPAT_IS_REINIT_COMPLETION -DHAVE_ETHER_ADDR_COPY -DHAVE_TIMECOUNTER_H

        # Check for netdev_rss_key_fill in Kernel Module.symvers
        KERN_HAS_NETDEV_RSS_KEY_FILL := $(shell grep -c netdev_rss_key_fill $(KSRC)/Module.symvers 2> /dev/null)
        ifeq ($(KERN_HAS_NETDEV_RSS_KEY_FILL),1)
                cflags += -DHAVE_NETDEV_RSS_KEY_FILL
        endif

        # Check for dst_get_neighbour in Kernel include/net/dst.h
        #KERN_HAS_DST_GET_NEIGHBOUR := $(shell grep dst_get_neighbour $(KSRC1)/include/net/dst.h 2> /dev/null)
        #ifneq ($(KERN_HAS_DST_GET_NEIGHBOUR),)
        #         cflags += -DHAVE_DST_GET_NEIGHBOUR
        #endif

        # include local dirs mlnx_ofed_X.X
        INC_DIR += -I$(NVMESH_SRC_DIR)/mlnx_ofed_$(OFED_VER)/include -I$(NVMESH_SRC_DIR)/mlnx_ofed_$(OFED_VER)/include/linux
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
                $(error OFED Source Dir not found provide with OFED_SRC_DIR=/path/to/compat-rdma-$(OFED_VER))
            endif
            ifeq ($(OFA_KERNEL),)
                # Not there
                $(error OFA Kernel Source Dir not found provide with OFA_KERNEL=/path/to/compat-rdma)
            endif
        endif

        INFO_OFED := OFA OFED $(OFED_VER) in $(OFED_SRC_DIR). Symbols from $(OFED_SYMVERS)

        cflags += -DCOMPAT_RDMA -DCOMPAT_RDMA_$(OFED_VER_MAJ)_$(OFED_VER_MIN) -DCONFIG_COMPAT_IS_KTHREAD
        cflags += -DOFED_VER_MAJ=$(OFED_VER_MAJ) -DOFED_VER_MIN=$(OFED_VER_MIN)
        INC_DIR += -I$(OFED_SRC_DIR)/drivers -I$(OFA_KERNEL)/include -I$(OFA_KERNEL)/include/linux
    else
        ifeq ($(OFED_VER_TYPE), none)
        # No OFED - Compile against Kernel Source
                INFO_OFED := No OFED. Building against Kernel $(KERN_VER)
                KERN_FILES_PATH := $(NVMESH_SRC_DIR)/kernels/$(KERN_VER)

                cflags += -DNO_OFED

                # Check whether there is struct gid_attr for ib_modify_gid and ib_query_gid.
                cflags += -DIB_HAS_GID_ATTR=$(shell grep -c gid_attr $(KSRC1)/include/rdma/ib_verbs.h 2> /dev/null)

                # Check whether the ib_create_send_mad accepts base version.
                CREATE_SEND_MAD_GREP_PATTERN := (?s)ib_create_send_mad\((.*?);
                cflags += -DIB_CREATE_MAD_NEEDS_BASE_VERSION=$(shell grep -Pzo '$(CREATE_SEND_MAD_GREP_PATTERN)' $(KSRC1)/include/rdma/ib_mad.h 2> /dev/null | grep -c base_version)

                # Check whether ib_create_cq takes a cq_attr struct as a parameter instead of individual fields
                CREATE_CQ_GREP_PATTERN := (?s)ib_create_cq\((.*?);
                cflags += -DIB_CREATE_CQ_NEEDS_CQ_ATTR=$(shell grep -Pzo '$(CREATE_CQ_GREP_PATTERN)' $(KSRC1)/include/rdma/ib_verbs.h 2> /dev/null | grep -c ib_cq_init_attr)

                # Check whether the ib_sa_path_rec_get has retries parameter
                IB_SA_PATH_REC_GET_GREP_PATTERN := (?s)ib_sa_path_rec_get\((.*?);
                cflags += -DIB_SA_PATH_REC_GET_NEEDS_RETRIES=$(shell grep -Pzo '$(IB_SA_PATH_REC_GET_GREP_PATTERN)' $(KSRC1)/include/rdma/ib_sa.h 2> /dev/null | grep -c retries)
        else
                # Unknown OFED
                $(error Unknown OFED $(OFED_VER_STRING))
        endif
    endif
endif

cflags += -Wall
cflags += -Werror=comment

# debug
# cflags += -g -O1 -DCONFIG_NVMEIB_DEBUG -DDEBUG -DMGMT_STATS
# cflags += -DCONFIG_NVMEIB_DEBUG -DDEBUG
# cflags += -O3 -DTAKE_STATS -DMGMT_STATS
# cflags += -O0 -DMGMT_STATS

cflags += -O3 -DMGMT_STATS
# cflags += -DUSE_SPONTANEUS_DETACH
# cflags += -DDEBUG_SUM
# cflags += -DDEBUG_DUP_TOPOLOGY
# cflags   += -DNVMEIBS_CLIENTS_PROC
# cflags += -DTAKE_STATS
# cflags += -O3
# cflags += -fno-inline-small-functions -DDEBUG
# cflags += -g -O0 -fno-inline-small-functions
# cflags += -DDEBUG_OVEREAGER
# cflags += -DDEBUG_UNCOMPLETED
# cflags += -DDEBUG_TOPO_CNTRS -DAUTONOMOUS_SYNCS_STATS
# cflags += -DDEBUG_TRANSFERS

# polling on client side instead of receive message
# cflags += -DUSE_RDMA_POLLING
#cflags+=-DHAVE_TIMECOUNTER_H -DMLNX_OFED_3_1 -DHAVE_NETDEV_RSS_KEY_FILL -DOFED_VER_MAJ=3 -DOFED_VER_MIN=1
cflags += -DTRACE_CPUID

ifneq ($(COMMIT_ID),)
cflags +=-DCOMMIT_ID=0x$(COMMIT_ID) -DCOMMIT_ID_STR=\"$(COMMIT_ID)\"
else
cflags += -DCOMMIT_ID=0x0 -DCOMMIT_ID_STR=\"0\"
endif

# if V = 1 we get a full trace of the compile command
V ?= 0

LINUX_INCLUDE='\
    $(INC_DIR) \
    -include $(autoconf_h) \
    $(KCONFIG_H) \
    $$(if $$(CONFIG_XEN),-D__XEN_INTERFACE_VERSION__=$$(CONFIG_XEN_INTERFACE_VERSION)) \
    $$(if $$(CONFIG_XEN),-I$$(srctree)/arch/x86/include/mach-xen) \
    -I$$(srctree)/arch/$$(SRCARCH)/include \
    -Iarch/$$(SRCARCH)/include/generated \
    -Iinclude \
    -I$$(srctree)/arch/$$(SRCARCH)/include/uapi \
    -Iarch/$$(SRCARCH)/include/generated/uapi \
    -I$$(srctree)/include \
    -I$$(srctree)/include/uapi \
    -Iinclude/generated/uapi \
    $$(if $$(KBUILD_SRC),-Iinclude2 -I$$(srctree)/include) \
    -I$$(srctree)/arch/$$(SRCARCH)/include \
    -Iarch/$$(SRCARCH)/include/generated \
    $(INC_DIR2)'

all:
	$(info ============== Build Configuration ================)
	$(info Branch: $(BRANCH_NAME) Commit: $(COMMIT_ID))
	$(info Kernel: $(KERN_VER))
	$(info OFED: $(INFO_OFED))
	$(info $(INFO_SERV_CLNT))
	$(info ===================================================)
	make -C $(KSRC) M=$(PWD) V=$(V) EXTRA_CFLAGS="$(cflags) $(EXTRA_CFLAGS)" \
        LINUXINCLUDE=$(LINUX_INCLUDE) \
        KBUILD_EXTRA_SYMBOLS="$(OFED_SYMVERS)" modules

clean:
	make -C $(KSRC) M=$(PWD) clean

