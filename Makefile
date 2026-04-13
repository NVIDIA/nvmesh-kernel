# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# Parent Makefile
#
MAKE_PID := $(shell echo $$PPID)
JOBS := $(shell ps T | sed -n 's@.*$(MAKE_PID).*$(MAKE).* \(-j\|--jobs=\) *\([0-9]*[0-9]*\).*@\1\2@p')
ifeq ($(JOBS),)
    JOBS = -j1
endif

ifneq ($(LLVM),)
    export CC=clang
    export LD=ld.lld
    export AR=llvm-ar
    export NM=llvm-nm
    export STRIP=llvm-strip
    export OBJCOPY=llvm-objcopy
    export OBJDUMP=llvm-objdump
    export OBJSIZE=llvm-size
    export READELF=llvm-readelf
    export HOSTCC=clang
    export HOSTCXX=clang++
    export HOSTAR=llvm-ar
    export HOSTLD=ld.lld
    cflags += -DLLVM
    cflags += -Wno-error=unknown-pragmas
endif

# our kernel modules clnt for client and srv for server
#obj-m += softroce/

#
# nconfig_* functions are used to generate .config during build this file
# enables passing configurations to external scripts being used during
# compilation
define nconfig_save
    echo "#### AUTO-GENERATED FILE do not edit" > .config
    echo "$(1)" | sed "s/\s*|\s*/\n/g" >> .config
endef

define nconfig_set
NCONFIG_$(1)=Y|
endef

define nconfig_unset
# NCONFIG_$(1) is not set|
endef

define nconfig_set_val
NCONFIG_$(1)=$(2)|
endef

# check_ib_core_symbols: Generate shell commands to check IB core module symbols
# Args: $(1) = module directory path, $(2) = symvers file path
define check_ib_core_symbols
set -e;\
for file in $(1)/*.ko*; do \
name=$$(basename $$file); \
base=$${name%.*}; \
echo "Checking module $$base symbols..."; scripts/compare_symvers.py --new ./Module.symvers --orig "$(2)" --module $$base; \
done;
endef

# archive_ib_core_modules: Archive IB core modules to a tarball
# Args: $(1) = module directory path, $(2) = symvers file path
define archive_ib_core_modules
find $(1) -type f \( -name '*.ko' -o -name '*.ko.*' -o -name '*.h' -o -name '*.c' -not -name '*.mod.c' -not -name '*.ko.cmd' \) \
-exec realpath --relative-to=$(1) {} \; | tar -zcf ib_core_modules.tar.gz -C $(1) -T -
endef

# setup_kernel_ib_core_modules: Setup Kernel IB core modules for building and checking
# Args: $(1) = module directory path, $(2) = description (e.g., "OFED 5.4" or "Kernel 6.12"), 
#       $(3) = symvers file path (optional)
# Sets: REL_IB_CORE_MOD_DIR, INFO_CORE_MOD, CHECK_IB_CORE_MOD, and updates obj-m and configs
# Note: Must be called with $(eval $(call setup_kernel_ib_core_modules,...))
define setup_kernel_ib_core_modules
REL_IB_CORE_MOD_DIR := $$(shell dirname "$(1)")
obj-m += $$(REL_IB_CORE_MOD_DIR)/
INFO_CORE_MOD := Building IB Core Modules for $(2) from $$(REL_IB_CORE_MOD_DIR)
configs += $$(call nconfig_set_val,IB_CORE_MOD_DIR,$$(REL_IB_CORE_MOD_DIR))
ARC_IB_CORE_MOD := $$(call archive_ib_core_modules,$$(REL_IB_CORE_MOD_DIR))
ifneq ($(3),)
    INFO_CORE_MOD += (Checked against $(3))
    CHECK_IB_CORE_MOD := $$(call check_ib_core_symbols,$$(REL_IB_CORE_MOD_DIR),$(3))
else
    INFO_CORE_MOD += (Not Checked!)
endif
endef

# check_ofed_ib_core_modules: Check for OFED modules and setup if found
# Args: none (uses OFED_FULL_VER, OFED_VER, and OFED_SYMVERS from the calling context)
# Note: Must be called with $(eval $(call check_ofed_ib_core_modules))
define check_ofed_ib_core_modules
$$(info OFED_FULL_VER $(OFED_FULL_VER) OFED_VER $(OFED_VER))
OFED_MODS_SRC_DIR := $$(firstword $$(wildcard $(NVMESH_SRC_DIR)/ofeds/$(OFED_FULL_VER) $$(wildcard $(NVMESH_SRC_DIR)/ofeds/$(OFED_VER))))
$$(info PWD $(PWD) OFED_MODS_SRC_DIR $$(OFED_MODS_SRC_DIR))
ifneq ($$(OFED_MODS_SRC_DIR),)
    $$(info OFED_MODS_SRC_DIR exists)
    OFED_MODS_MAKEFILE := $$(wildcard $$(OFED_MODS_SRC_DIR)/drivers/infiniband/core/Makefile)
    ifneq ($$(OFED_MODS_MAKEFILE),)
        $$(info OFED_MODS_MAKEFILE $$(OFED_MODS_MAKEFILE) exists)
        REL_IB_CORE_MOD_DIR := $$(patsubst $(NVMESH_SRC_DIR)/%,%,$$(shell dirname "$$(OFED_MODS_MAKEFILE)"))
        obj-m += $$(REL_IB_CORE_MOD_DIR)/
        INFO_CORE_MOD := Building IB Core Modules for OFED $$(OFED_FULL_VER) from $$(REL_IB_CORE_MOD_DIR)
        configs += $$(call nconfig_set_val,IB_CORE_MOD_DIR,$$(REL_IB_CORE_MOD_DIR))
        ARC_IB_CORE_MOD := $$(call archive_ib_core_modules,$$(REL_IB_CORE_MOD_DIR))
        ifneq ($(OFED_SYMVERS),)
            INFO_CORE_MOD += (Checked against $(OFED_SYMVERS))
            CHECK_IB_CORE_MOD := $$(call check_ib_core_symbols,$$(REL_IB_CORE_MOD_DIR),$(OFED_SYMVERS))
        else
            INFO_CORE_MOD += (Not Checked!)
        endif
    else
        INFO_CORE_MOD := NOT Building IB Core Modules for $$(OFED_FULL_VER) - $$(OFED_MODS_SRC_DIR)/drivers/infiniband/core/Makefile not found
    endif
else
    INFO_CORE_MOD := NOT Building IB Core Modules for $$(OFED_FULL_VER) - ofeds/$$(OFED_FULL_VER) or ofeds/$$(OFED_VER) not found
endif
endef

ifeq ($(VERBOSE),false)
    VV=@
    configs+=$(call nconfig_unset,VERBOSE)
else
    VV=
    configs+=$(call nconfig_set,VERBOSE)
endif

# Change to YES to enable core unitest
CORE_UNITEST ?= no
COMPILE_CLIENT =
COMPILE_COMMON =
ifdef IM_CLIENT
    COMPILE_CLIENT = yes
else
    ifdef IM_BOTH
        COMPILE_CLIENT = yes
    endif
endif

ifeq ($(COMPILE_CLIENT),yes)
    COMPILE_COMMON = yes
    export CORE_UNITEST
endif

COMPILE_SERVER =
ifdef IM_SERVER
    COMPILE_COMMON = yes
    COMPILE_SERVER = yes
else
    ifdef IM_BOTH
        COMPILE_COMMON = yes
        COMPILE_SERVER = yes
    endif
endif

PY_TO_EXEC_VER ?= "3.10"
ifeq ($(CREATE_PYTHON_TOOLS_EXEC),yes)
    PY_TO_EXEC = PY=$(PY_TO_EXEC_VER) ./py_to_exec.sh
    PY_TO_EXEC_INFO = Building python tools as executables with python $(PY_TO_EXEC_VER)
else
    PY_TO_EXEC = ./py_to_symlink.sh
    PY_TO_EXEC_INFO = Building python tools as symlinks to python sources
endif

BUILD_KERNEL_MODULES = yes
ifneq ($(COMPILE_CLIENT),)
    obj-m += clnt/atom/
    obj-m += clnt/
    ifdef IM_CLIENT
        INFO_SERV_CLNT = Building Client ONLY

        INFO_TOMA = NOT building TOMA
        COMPILE_TOMA =
        CLEAN_TOMA =

        ifdef MK_RPM
            INFO_RPM = Making Client RPM ONLY
            BUILD_RPM = ./mk_client_rpm $(KERN_VER) $(OFED_VER_STRING) $(CREATE_PYTHON_TOOLS_EXEC)
        else
            INFO_RPM = NOT Making RPM
            BUILD_RPM =
        endif
    endif
else
    INFO_SERV_CLNT = Not building Kenrel modules
    INFO_TOMA = NOT building TOMA
    COMPILE_TOMA =
    CLEAN_TOMA =
    BUILD_KERNEL_MODULES =
    ifdef MK_RPM
        INFO_RPM = Making Base RPM ONLY
        BUILD_RPM = ./mk_base_rpm $(KERN_VER) $(OFED_VER_STRING) $(CREATE_PYTHON_TOOLS_EXEC)
    else
        INFO_RPM = NOT Making RPM
        BUILD_RPM =
    endif
endif

# export MOD=release
# the supported TOMA compile modes:
# 1. debug = TCMD
# 2. release = TCMR
# 3. delease = TCMDR
# 4. both debug & release = TCMB
# 5. all = TCMA
# 6. udp only compilation = TCMUO
ifndef TCM
    TCM = TCMR
endif

GEN_USED_SYMVERS = ./bin/nvmesh_gen_symvers
COMPILE_UTILS = cd perfTest/io_stress/di_parser; $(MAKE) CFLAGS="$(UTILSFLAGS)" SSDA=$(NVMESH_SRC_DIR); cd ../scan_locks; $(MAKE) SSDA=$(NVMESH_SRC_DIR); cd ../change_bio_in_air; $(MAKE) all; cd ../cmp_blocks;  $(MAKE) CFLAGS="$(UTILSFLAGS)" SSDA=$(NVMESH_SRC_DIR); cd ../gen_md;  $(MAKE) CFLAGS="$(UTILSFLAGS)" SSDA=$(NVMESH_SRC_DIR); cd ../../..
CLEAN_UTILS =   cd perfTest/io_stress/di_parser; $(MAKE) SSDA=$(NVMESH_SRC_DIR) clean; cd ../scan_locks; $(MAKE) SSDA=$(NVMESH_SRC_DIR) clean; cd ../change_bio_in_air; $(MAKE) clean; cd ../cmp_blocks; $(MAKE) SSDA=$(NVMESH_SRC_DIR) clean; cd ../gen_md; $(MAKE) SSDA=$(NVMESH_SRC_DIR) clean; cd ../../..
COMPRESS_KERNEL_MODULES =

ifeq ($(COMPRESS_KO),yes)
    COMPRESS_KERNEL_MODULES = RPM/compress_kernel_modules.sh
    ifdef MK_RPM
        COMPRESS_KERNEL_MODULES = RPM/compress_kernel_modules.sh
    endif
    ifeq ($(IS_COMPILATOR),true)
        COMPRESS_KERNEL_MODULES = RPM/compress_kernel_modules.sh
    endif
endif

ifneq ($(COMPILE_SERVER),)
    obj-m += srv/
    COMPILE_TOOLS= cd utils && $(MAKE) $(JOBS) all
    CLEAN_TOOLS= cd utils && $(MAKE) clean

    ifeq ($(TCM), TCMD)
        INFO_TOMA = Building TOMA in DEBUG mode
        COMPILE_TOMA = cd toma && $(MAKE) $(JOBS) all $(SECTOR_SHIFT_FLAG)
        CLEAN_TOMA = cd toma; $(MAKE) clean
    else
        ifeq ($(TCM), TCMR)
            INFO_TOMA = Building TOMA in RELEASE mode
            COMPILE_TOMA = cd toma && $(MAKE) $(JOBS) all MOD=release $(SECTOR_SHIFT_FLAG)
            CLEAN_TOMA = cd toma; $(MAKE) clean MOD=release
        else
            ifeq ($(TCM), TCMDR)
                INFO_TOMA = Building TOMA in DEBUG RELEASE mode
                COMPILE_TOMA = cd toma && $(MAKE) $(JOBS) all MOD=release DEBUG=yes $(SECTOR_SHIFT_FLAG)
                CLEAN_TOMA = cd toma; $(MAKE) clean MOD=release DEBUG=yes
            else
                INFO_TOMA = Building TOMA in IB and UDP-only (release) modes
                COMPILE_TOMA = cd toma && $(MAKE) $(JOBS) all MOD=release $(SECTOR_SHIFT_FLAG)
                CLEAN_TOMA = cd toma; $(MAKE) clean; $(MAKE) clean MOD=release; $(MAKE) clean MOD=release DEBUG=yes
            endif
        endif
    endif

    ifndef IM_SERVER
        INFO_SERV_CLNT = Building BOTH Server and Client

        ifdef MK_RPM
            INFO_RPM = Making BOTH RPMs
            BUILD_RPM = ./mk_local_rpm $(KERN_VER) $(OFED_VER_STRING) $(CREATE_PYTHON_TOOLS_EXEC)
        else
            INFO_RPM = NOT Making RPM
            BUILD_RPM =
        endif
    else
        INFO_SERV_CLNT = Building Server ONLY

        ifdef MK_RPM
            INFO_RPM = Making Server RPM ONLY
            BUILD_RPM = ./mk_target_rpm $(KERN_VER) $(OFED_VER_STRING) $(CREATE_PYTHON_TOOLS_EXEC)
        else
            INFO_RPM = NOT Making RPM
            BUILD_RPM =
        endif
    endif
endif

# tests begin
INFO_TEST = NOT Building Tests
ifdef IM_TEST
    COMPILE_COMMON = yes
    INFO_TEST = Building PCIe_Atomic Tests
    obj-m += testing/pcie_atomic/
endif
ifdef IM_TEST_T
    COMPILE_COMMON = yes
    INFO_TEST = Building Threads Tests
    obj-m += testing/threads/
endif
ifdef IM_TEST_C
    COMPILE_COMMON = yes
    INFO_TEST = Building Context Tests
    obj-m += testing/context/
endif
ifdef IM_TEST_TCP
    COMPILE_COMMON = yes
    INFO_TEST = Building TCP Tests
    obj-m += testing/tcp_sock/
endif
# tests end

ifeq ($(COMPILE_COMMON),yes)
    obj-m += common/
    obj-m += common_public/
    obj-m += keeper/
endif


# the kernel sources
ifeq ($(KERN_VER),)
        KERN_VER := $(shell uname -r)
endif

KERN_ARCH := $(shell uname -m)

ifneq ($(_KSRC),)
    KSRC = $(_KSRC)
else
    KSRC := /lib/modules/$(KERN_VER)/build
endif
export KSRC

ifneq ($(_KSRC1),)
    KSRC1 = $(_KSRC1)
else
    KSRC1 := $(wildcard /lib/modules/$(KERN_VER)/source)
    # Check is /lib/modules source link exists. Not all distro's have this e.g. Ubuntu
    ifeq ($(KSRC1),)
        KSRC1 := $(KSRC)
    endif
endif
export KSRC1

ifeq ($(MODVERSIONS),)
    ifneq ($(shell grep -i "CONFIG_MODVERSIONS=y" $(KSRC)/.config 2> /dev/null),)
        configs+=$(call nconfig_set,MODVERSIONS)
        MODVERSIONS=1
    else
        configs+=$(call nconfig_unset,MODVERSIONS)
        MODVERSIONS=0
    endif
endif

ifneq ($(wildcard $(KSRC1)/include/linux/sched/mm.h),)
    cflags += -DKSRC_INCLUDE_SCHED_MM=1
else
    cflags += -DKSRC_INCLUDE_SCHED_MM=0
endif

# Check for Broadcom Netxtreme Support
ifneq ($(BNXT_DIR),)
    configs+=$(call nconfig_set,BNXT)
    cflags += -DBNXT_RE=1
    INFO_BNXT := bnxt from $(BNXT_DIR)

    # Check Broadcom Compatibility
    ifneq ($(shell grep "RDMA_CORE_CAP_PROT_ROCE_UDP_ENCAP" $(KSRC1)/include/rdma/ib_verbs.h 2> /dev/null),)
        BNXT_DISTRO_CFLAG += -DENABLE_SHADOW_QP -DENABLE_ROCEV2_QP1
    endif

    ifneq ($(shell grep "IB_ZERO_BASED" $(KSRC1)/include/rdma/ib_verbs.h > /dev/null 2>&1 && echo zero),)
        BNXT_DISTRO_CFLAG += -DHAVE_IB_ZERO_BASED
    endif

    ifneq ($(shell grep "IB_ACCESS_ON_DEMAND" $(KSRC1)/include/rdma/ib_verbs.h > /dev/null 2>&1 && echo demand),)
        BNXT_DISTRO_CFLAG += -DHAVE_IB_ACCESS_ON_DEMAND
    endif

    ifneq ($(shell grep "alloc_mr" $(KSRC1)/include/rdma/ib_verbs.h > /dev/null 2>&1 && echo alloc_mr),)
        BNXT_DISTRO_CFLAG += -DHAVE_IB_ALLOC_MR
    endif

    ifneq ($(shell grep -so "ib_mw_type" $(KSRC1)/include/rdma/ib_verbs.h > /dev/null 2>&1 && echo ib_mw_type),)
        BNXT_DISTRO_CFLAG += -DHAVE_IB_MW_TYPE
    endif

    ifneq ($(shell grep -o "PKT_HASH_TYPE" $(KSRC1)/include/linux/skbuff.h),)
        BNXT_DISTRO_CFLAG += -DHAVE_SKB_HASH_TYPE
    endif

    ifneq ($(shell grep -o "ether_addr_copy" $(KSRC1)/include/linux/etherdevice.h),)
        BNXT_DISTRO_CFLAG += -DHAVE_ETHER_ADDR_COPY
    endif

    ifneq ($(shell grep -o "NETDEV_BONDING_INFO" $(KSRC1)/include/linux/netdevice.h),)
        BNXT_DISTRO_CFLAG += -DHAVE_NETDEV_BONDING_INFO -DHAVE_ROCE_LAG_SUPPORT
    endif

    BNXT_CFLAGS := ${BNXT_DISTRO_CFLAG} -DFPGA -g -DCONFIG_BNXT_SRIOV -DCONFIG_BNXT_DCB -DHAVE_NET_VERSION -DENABLE_DEBUGFS -DCONFIG_BNXT_RE -DENABLE_SHADOW_QP -DENABLE_ROCE_TOS -DBIND_MW_FENCE_WQE

    $(info BNXT_CFLAGS=$(BNXT_CFLAGS))

    obj-m += common_public/bnxt/

    BNXT_SYMVERS := $(BNXT_DIR)/bnxt_re/Module.symvers
else
    configs+=$(call nconfig_unset,BNXT)
    cflags += -DBNXT_RE=0
endif

ifeq ($(M),)
    NVMESH_SRC_DIR := $(shell pwd)
else
    NVMESH_SRC_DIR := $(M)
endif

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


ifeq ($(wildcard $(KSRC1)/arch/x86/include/asm/i387.h),)
    # i387.h does not exist
    cflags += -DKS_HAS_I387_HEADER=0
else
    cflags += -DKS_HAS_I387_HEADER=1
endif


ifeq ($(wildcard $(KSRC1)/include/linux/hashtable.h),)
    # hashtable.h does not exist
    cflags += -DKS_HASHTABLE=0
else
    ifeq ($(shell grep -w "define hash_for_each_possible" $(KSRC1)/include/linux/hashtable.h | grep "name, obj, node, member, key" 2> /dev/null),)
        # four argument hash_for_each_possible
        cflags += -DKS_HASHTABLE=1
    else
        # five argument hash_for_each_possible - use NVIDIA implementation
        cflags += -DKS_HASHTABLE=0
    endif
endif
ifeq ($(shell grep reinit_completion $(KSRC1)/include/linux/completion.h 2> /dev/null),)
    # reinit_completion is not defined
    cflags += -DKS_REINIT_COMPLETION=0
else
    cflags += -DKS_REINIT_COMPLETION=1
endif

ifneq ($(shell grep "register_netdevice_notifier_rh" $(KSRC1)/include/linux/netdevice.h 2> /dev/null),)
    cflags +=-DKS_HAVE_REGISTER_NETDEVICE_NOTIFIER_RH=1
else
    cflags +=-DKS_HAVE_REGISTER_NETDEVICE_NOTIFIER_RH=0
endif

ifneq ($(shell grep "bio_is_rw" $(KSRC1)/include/linux/bio.h 2> /dev/null),)
    cflags += -DKS_NO_BIO_IS_RW=0
else
    cflags += -DKS_NO_BIO_IS_RW=1
endif

ifneq ($(shell grep "*fault.*struct vm_area_struct" $(KSRC1)/include/linux/mm.h 2> /dev/null),)
    cflags +=-DKS_FAULT_EXPECTS_VM_AREA=1
else
    cflags +=-DKS_FAULT_EXPECTS_VM_AREA=0
endif

ifneq ($(shell find $(KSRC1)/include/linux/sched/ -type f -name signal.h 2> /dev/null),)
    cflags +=-DKS_HAS_SCHED_SIGNAL_HEADER=1
else
    cflags +=-DKS_HAS_SCHED_SIGNAL_HEADER=0
endif
ifneq ($(shell find $(KSRC1)/include/linux/sched/ -type f -name task.h 2> /dev/null),)
    cflags +=-DKS_HAS_SCHED_TASK_HEADER=1
else
    cflags +=-DKS_HAS_SCHED_TASK_HEADER=0
endif

ifneq ($(shell grep -w bitmap_scnprintf $(KSRC1)/include/linux/bitmap.h 2> /dev/null),)
    cflags += -DKS_HAS_BITMAP_SCNPRINTF=1
else
    cflags += -DKS_HAS_BITMAP_SCNPRINTF=0
endif

ifneq ($(shell grep -w uuid_be_gen $(KSRC1)/include/linux/uuid.h 2> /dev/null),)
    cflags += -DKS_HAS_UUID_BE_GEN=1
else
    cflags += -DKS_HAS_UUID_BE_GEN=0
endif

ifneq ($(shell grep -w "struct sa_path_rec" $(KSRC1)/include/rdma/ib_sa.h 2> /dev/null),)
    cflags += -DKS_HAS_SA_PATH_REC=1
else
    cflags += -DKS_HAS_SA_PATH_REC=0
endif

ifneq ($(shell grep -w "function" $(KSRC1)/include/linux/timer.h $(KSRC1)/include/linux/timer_types.h | grep -w "struct timer_list" 2> /dev/null),)
    cflags += -DKS_NEW_TIMER_API=1
else
    cflags += -DKS_NEW_TIMER_API=0
endif

ifeq ($(wildcard $(KSRC1)/include/linux/genhd.h),)
    cflags +=-DKS_HAS_GENHD_H=0
else
    cflags +=-DKS_HAS_GENHD_H=1

    ifneq ($(shell grep "driverfs_dev" $(KSRC1)/include/linux/genhd.h 2> /dev/null),)
        cflags += -DKS_DRIVERFS_DEV=1
    else
        cflags += -DKS_DRIVERFS_DEV=0
    endif

    ifneq ($(shell grep -w "void part_inc_in_flight" $(KSRC1)/include/linux/genhd.h | grep -w "struct request_queue" 2> /dev/null),)
        cflags += -DKS_PART_INC_IN_FLIGHT_USES_Q=1
    else
        cflags += -DKS_PART_INC_IN_FLIGHT_USES_Q=0
    endif

    ifneq ($(shell grep -w "void part_dec_in_flight" $(KSRC1)/include/linux/genhd.h | grep -w "struct request_queue" 2> /dev/null),)
        cflags += -DKS_PART_DEC_IN_FLIGHT_USES_Q=1
    else
        cflags += -DKS_PART_DEC_IN_FLIGHT_USES_Q=0
    endif
endif

ifneq ($(shell grep -w "sme_active" $(KSRC1)/Module.symvers | grep -w "EXPORT_SYMBOL_GPL" 2> /dev/null),)
    cflags += -DKS_HAS_GPL_SME_ACTIVE=1
else
    cflags += -DKS_HAS_GPL_SME_ACTIVE=0
endif

ib_sa_path_rec_get_match := '(?s)int\s+ib_sa_path_rec_get\s*\([^\)]+\)'
ifneq ($(shell grep -Poz \'$(ib_sa_path_rec_get_match)\' $(KSRC1)/include/rdma/ib_sa.h | grep retries 2> /dev/null),)
    cflags += -DKS_IB_SA_PATH_REC_GET_HAS_RETRIES=1
else
    cflags += -DKS_IB_SA_PATH_REC_GET_HAS_RETRIES=0
endif

ifneq ($(shell find $(KSRC1)/include/linux/ -type f -name irq_poll.h 2> /dev/null),)
    cflags +=-DKS_HAS_IRQ_POLL=1
else
    cflags +=-DKS_HAS_IRQ_POLL=0
endif

ifneq ($(shell grep -w "vm_fault_t" $(KSRC1)/include/linux/mm_types.h 2> /dev/null),)
    cflags +=-DKS_HAS_VM_FAULT_T=1
else
    cflags +=-DKS_HAS_VM_FAULT_T=0
endif

ifneq ($(shell grep -w 'define mmiowb()' $(KSRC1)/arch/x86/include/asm/io.h 2> /dev/null),)
    cflags +=-DKS_HAS_MMIOWB=1
else
    cflags +=-DKS_HAS_MMIOWB=0
endif

ifneq ($(shell grep -w '__mutex_owner' $(KSRC1)/include/linux/mutex.h 2> /dev/null),)
    cflags +=-DKS_HAS_MUTEX_OWNER=1
else
    cflags +=-DKS_HAS_MUTEX_OWNER=0
endif

ifneq ($(shell grep -w 'SO_INCOMING_CPU' $(KSRC1)/include/uapi/asm-generic/socket.h 2> /dev/null),)
    cflags += -DKS_HAS_SO_INCOMING_CPU=1
else
    cflags += -DKS_HAS_SO_INCOMING_CPU=0
endif

ifneq ($(wildcard $(KSRC1)/include/linux/sockptr.h),)
    cflags += -DKS_HAS_KERNEL_SOCKPTR=1
else
    cflags += -DKS_HAS_KERNEL_SOCKPTR=0
endif

HAS_DO_GETTIMEOFDAY=0
ifneq ($(shell grep -w 'void do_gettimeofday' $(KSRC1)/include/linux/time.h 2> /dev/null),)
    HAS_DO_GETTIMEOFDAY=1
endif
ifneq ($(shell grep -w 'void do_gettimeofday' $(KSRC1)/include/linux/timekeeping.h 2> /dev/null),)
	HAS_DO_GETTIMEOFDAY=1
endif
ifneq ($(shell grep -w 'void do_gettimeofday' $(KSRC1)/include/linux/timekeeping32.h 2> /dev/null),)
    HAS_DO_GETTIMEOFDAY=1
endif
ifeq ($(HAS_DO_GETTIMEOFDAY), 1)
    cflags +=-DKS_HAS_DO_GETTIMEOFDAY=1
else
    cflags +=-DKS_HAS_DO_GETTIMEOFDAY=0
endif
ifneq ($(shell grep -w 'void getnstimeofday' $(KSRC1)/include/linux/timekeeping32.h 2> /dev/null),)
    HAS_GETNSTIMEOFDAY=1
endif
ifeq ($(HAS_GETNSTIMEOFDAY), 1)
    cflags +=-DKS_HAS_GETNSTIMEOFDAY=1
else
    cflags +=-DKS_HAS_GETNSTIMEOFDAY=0
endif

ifneq ($(shell grep -w 'int atomic_inc_not_zero_hint' $(KSRC1)/include/linux/atomic.h 2> /dev/null),)
    cflags +=-DKS_HAS_ATOMIC_INC_NOT_ZERO_HINT=1
else
    cflags +=-DKS_HAS_ATOMIC_INC_NOT_ZERO_HINT=0
endif

ifeq ($(wildcard $(KSRC1)/include/scsi/scsi_request.h),)
    cflags +=-DKS_HAS_SCSCI_REQUEST_H=0
else
    cflags +=-DKS_HAS_SCSCI_REQUEST_H=1
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
        OFED_DKMS_VERS := $(shell ofed_info -l | grep mlnx-ofed-kernel-dkms | awk '{print $$3}')
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

        # Mellanox OFED 3.X
        cflags += -DCONFIG_COMPAT_IS_REINIT_COMPLETION -DHAVE_ETHER_ADDR_COPY

        # Check if kernel has timecounter.h
        ifneq ($(wildcard $(KSRC1)/include/linux/timecounter.h),)
            cflags += -DHAVE_TIMECOUNTER_H
        endif

        cflags += -DHAVE_LINUX_PRINTK_H

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
        INC_DIR += -I$(shell pwd)/mlnx_ofed_$(OFED_VER)/include -I$(shell pwd)/mlnx_ofed_$(OFED_VER)/include/linux
            # Check if kernel has __ib_alloc_pd
            ifneq ($(shell grep __ib_alloc_pd $(OFED_SRC_DIR)/include/rdma/ib_verbs.h 2> /dev/null),)
                cflags += -DKS_HAS_IB_ALLOC_MACRO=1
                ifneq ($(shell grep -A 1 __ib_alloc_pd $(OFED_SRC_DIR)/include/rdma/ib_verbs.h | grep skip_tracking 2> /dev/null),)
                    cflags += -DKS_IB_ALLOC_HAS_SKIP_TRACKING=1
                else
                    cflags += -DKS_IB_ALLOC_HAS_SKIP_TRACKING=0
                endif
            else
                cflags += -DKS_HAS_IB_ALLOC_MACRO=0
            endif
            ifneq ($(shell grep ib_get_dma_mr $(OFED_SRC_DIR)/include/rdma/ib_verbs.h 2> /dev/null),)
                cflags += -DHAS_IB_GET_DMA_MR=1
            else
                cflags += -DHAS_IB_GET_DMA_MR=0
            endif
    endif
    # enable HAVE_CGROUP_RDMA_H for OFED >= 4.1
    ifneq ($(wildcard $(OFED_SRC_DIR)/include/linux/cgroup_rdma.h),)
        ifneq ($(wildcard $(KSRC1)/include/linux/cgroup_rdma.h),)
            cflags += -DHAVE_CGROUP_RDMA_H
        endif
    endif
    ifneq ($(shell grep -w ib_uses_virt_dma $(OFED_SRC_DIR)/include/rdma/ib_verbs.h 2> /dev/null),)
        cflags += -DKS_HAS_VIRT_DMA_SUPPORT=1
    else
        cflags += -DKS_HAS_VIRT_DMA_SUPPORT=0
    endif

    ifneq ($(shell grep ib_query_gid $(OFED_SRC_DIR)/include/rdma/ib_verbs.h 2> /dev/null),)
        cflags += -DHAS_IB_QUERY_GID=1
    else
        cflags += -DHAS_IB_QUERY_GID=0
    endif

    ifneq ($(shell grep -w kref_read $(OFED_SRC_DIR)/include/linux/kref.h 2> /dev/null),)
        cflags +=-DKS_HAS_KREF_READ=1
    else
        ifneq ($(shell grep -w kref_read $(KSRC1)/include/linux/kref.h 2> /dev/null),)
            cflags +=-DKS_HAS_KREF_READ=1
        endif
    endif

    # Set a flag if netdev_has_upper_dev_all_rcu is used in core_priv.h but is not defined in the kernel
    ifneq ($(shell grep netdev_has_upper_dev_all_rcu $(OFED_SRC_DIR)/drivers/infiniband/core/core_priv.h 2> /dev/null),)
        ifeq ($(shell grep netdev_has_upper_dev_all_rcu $(KSRC)/Module.symvers 2> /dev/null),)
            cflags += -DKS_USES_NETDEV_HAS_UPPER_DEV_ALL_RCU=1
        endif
    endif

    # Check if cma_priv.h exists
    ifneq ($(wildcard $(OFED_SRC_DIR)/drivers/infiniband/core/cma_priv.h),)
        cflags += -DIB_HAS_CMA_PRIV_H=1
    else
        ifneq ($(wildcard $(OFED_SRC_DIR)/drivers/infiniband/core/cma.c),)
            #If cma_priv.h does not exist, try and generate it from cma.c
            cma_priv_dir := /tmp/__cma_priv_dir
            $(shell mkdir -p $(cma_priv_dir)/infiniband/core)
            $(shell echo '#include <rdma/rdma_cm.h>' > $(cma_priv_dir)/infiniband/core/cma_priv.h)
            # Use sed to extract definition of struct rdma_id_private from cma.c
            # It works by extracting all lines between:
            # ^struct rdma_id_private {
            # and
            # ^};
            $(shell sed -rn '/^struct\s+rdma_id_private\s+\{/,/^};/p' $(OFED_SRC_DIR)/drivers/infiniband/core/cma.c >> $(cma_priv_dir)/infiniband/core/cma_priv.h)
            $(info created cma_priv.h in $(cma_priv_dir)/infiniband/core/)
            cflags += -DIB_HAS_CMA_PRIV_H=1
            INC_DIR += -I$(cma_priv_dir)
        endif
    endif

    $(eval $(call check_ofed_ib_core_modules))
    $(info obj-m $(obj-m))
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
        # Check if kernel has __ib_alloc_pd
        ifneq ($(shell grep __ib_alloc_pd $(OFED_SRC_DIR)/include/rdma/ib_verbs.h 2> /dev/null),)
            cflags += -DKS_HAS_IB_ALLOC_MACRO=1
            ifneq ($(shell grep -A 1 __ib_alloc_pd $(OFED_SRC_DIR)/include/rdma/ib_verbs.h | grep skip_tracking 2> /dev/null),)
                cflags += -DKS_IB_ALLOC_HAS_SKIP_TRACKING=1
            else
                cflags += -DKS_IB_ALLOC_HAS_SKIP_TRACKING=0
            endif
        else
            cflags += -DKS_HAS_IB_ALLOC_MACRO=0
        endif
        ifneq ($(shell grep ib_get_dma_mr $(OFED_SRC_DIR)/include/rdma/ib_verbs.h 2> /dev/null),)
            cflags += -DHAS_IB_GET_DMA_MR=1
        else
            cflags += -DHAS_IB_GET_DMA_MR=0
        endif

        $(eval $(call check_ofed_ib_core_modules))
        $(info obj-m $(obj-m))
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

            # Check whether to define KS_MLX5. Its' backported to some kernels so we can't use the version
            DIR := $(wildcard $(KERN_FILES_PATH)/drivers/infiniband/hw/mlx5/mlx5_ib.h)
            ifneq ($(DIR),)
                cflags += -DKS_MLX5=1
            endif

            #Check whether mlx5_ib_wq has seperate swr_ctx
            ifneq ($(shell grep swr_ctx $(KERN_FILES_PATH)/drivers/infiniband/hw/mlx5/mlx5_ib.h 2> /dev/null),)
                cflags += -DMLX5_IB_WQ_SWR_CTX=1
            else
                cflags += -DMLX5_IB_WQ_SWR_CTX=0
            endif

            #Check whether mlx5_ib_qp has 'struct mlx5_frag_buf' or 'struct mlx5_buf'
            ifneq ($(shell grep -Poz '(?s)struct\s+mlx5_ib_qp\s+\{.*?(?=\n\};)\n\};\n' $(KERN_FILES_PATH)/drivers/infiniband/hw/mlx5/mlx5_ib.h | grep -a "struct mlx5_frag_buf" 2> /dev/null),)
                cflags += -DMLX5_IB_QP_FRAG_BUF=1
                # Check whether mlx5_ib_wq has 'struct mlx5_frag_buf_ctrl'
                ifneq ($(shell grep -Poz '(?s)struct\s+mlx5_ib_wq\s+\{.*?(?=\n\};)\n\};\n' $(KERN_FILES_PATH)/drivers/infiniband/hw/mlx5/mlx5_ib.h | grep -a "struct mlx5_frag_buf_ctrl" 2> /dev/null),)
                    cflags += -DMLX5_IB_WQ_FRAG_BUF_CTRL=1
                else
                    cflags += -DMLX5_IB_WQ_FRAG_BUF_CTRL=0
                endif
            else
                cflags += -DMLX5_IB_QP_FRAG_BUF=0 -DMLX5_IB_WQ_FRAG_BUF_CTRL=0
            endif
            #Check whether mlx5_ib_cq_buf has 'struct mlx5_frag_buf_ctrl'
            ifneq ($(shell grep -Poz '(?s)struct\s+mlx5_ib_cq_buf\s+\{.*?(?=\n\};)\n\};\n' $(KERN_FILES_PATH)/drivers/infiniband/hw/mlx5/mlx5_ib.h | grep -a "struct mlx5_frag_buf_ctrl" 2> /dev/null),)
                cflags += -DMLX5_IB_CQ_FRAG_BUF_CTRL=1
            else
                cflags += -DMLX5_IB_CQ_FRAG_BUF_CTRL=0
            endif

            #Check whether ib_device has a get_netdev fn pointer. It's backported to some kernels so we can't use the version
            ifneq ($(shell grep get_netdev $(KSRC1)/include/rdma/ib_verbs.h 2> /dev/null),)
                cflags += -DKS_IB_DEVICE_HAS_GET_NETDEV=1
            else
                cflags += -DKS_IB_DEVICE_HAS_GET_NETDEV=0
            endif
            ifneq ($(shell grep -w ib_uses_virt_dma $(KSRC1)/include/rdma/ib_verbs.h 2> /dev/null),)
                cflags += -DKS_HAS_VIRT_DMA_SUPPORT=1
            else
                cflags += -DKS_HAS_VIRT_DMA_SUPPORT=0
            endif
            # Check if kernel has __ib_alloc_pd
            ifneq ($(shell grep __ib_alloc_pd $(KSRC1)/include/rdma/ib_verbs.h 2> /dev/null),)
                cflags += -DKS_HAS_IB_ALLOC_MACRO=1
                ifneq ($(shell grep -A 1 __ib_alloc_pd $(KSRC1)/include/rdma/ib_verbs.h | grep skip_tracking 2> /dev/null),)
                    cflags += -DKS_IB_ALLOC_HAS_SKIP_TRACKING=1
                else
                    cflags += -DKS_IB_ALLOC_HAS_SKIP_TRACKING=0
                endif
            else
                cflags += -DKS_HAS_IB_ALLOC_MACRO=0
            endif
            ifneq ($(shell grep ib_get_dma_mr $(KSRC1)/include/rdma/ib_verbs.h 2> /dev/null),)
                cflags += -DHAS_IB_GET_DMA_MR=1
            else
                cflags += -DHAS_IB_GET_DMA_MR=0
            endif

            ifneq ($(shell grep "struct mlx5_bf[[:space:]]*bf" $(KERN_FILES_PATH)/drivers/infiniband/hw/mlx5/mlx5_ib.h 2> /dev/null),)
                cflags += -DIB_MLX5_NEW_BF=1
            endif

            ifneq ($(shell grep -w kref_read $(KSRC1)/include/linux/kref.h 2> /dev/null),)
                cflags +=-DKS_HAS_KREF_READ=1
            else
                cflags +=-DKS_HAS_KREF_READ=0
            endif

            # Check if ah_attr requires type field
            ifneq ($(shell grep -w rdma_ah_attr_type $(KSRC1)/include/rdma/ib_verbs.h 2> /dev/null),)
                cflags +=-DKS_IB_HAS_RDMA_AH_ATTR_TYPE=1
            else
                cflags +=-DKS_IB_HAS_RDMA_AH_ATTR_TYPE=0
            endif

            ifneq ($(wildcard $(KERN_FILES_PATH)/drivers/infiniband/core/cma_priv.h),)
                cflags += -DIB_HAS_CMA_PRIV_H=1
            else
                ifneq ($(wildcard $(KERN_FILES_PATH)/drivers/infiniband/core/cma.c),)
                    # If cma_priv.h does not exist, try and generate it from cma.c
                    cma_priv_dir := /tmp/__cma_priv_dir
                    $(shell mkdir -p $(cma_priv_dir)/infiniband/core)
                    $(shell echo '#include <rdma/rdma_cm.h>' > $(cma_priv_dir)/infiniband/core/cma_priv.h)
                    # Use sed to extract definition of struct rdma_id_private from cma.c
                    # It works by extracting all lines between:
                    # ^struct rdma_id_private {
                    # and
                    # ^};
                    $(shell sed -rn '/^struct\s+rdma_id_private\s+\{/,/^};/p' $(KERN_FILES_PATH)/drivers/infiniband/core/cma.c >> $(cma_priv_dir)/infiniband/core/cma_priv.h)
                    $(info created cma_priv.h in $(cma_priv_dir)/infiniband/core/)
                    INC_DIR += -I$(cma_priv_dir)
                    cflags += -DIB_HAS_CMA_PRIV_H=1
                endif
            endif

            ifneq ($(wildcard $(KERN_FILES_PATH)/drivers/infiniband/core/Makefile),)
                # If kernel symvers can be found, check the patched modules have the same symbols
                # RHEL kernels store symvers in /boot/symvers-<kernel-version> or /boot/symvers-<kernel-version>.gz
                # Ubuntu kernels store symvers in /usr/src/linux-headers-<kernel-version>/Module.symvers
                KERN_SYMVERS = $(firstword \
                    $(wildcard /boot/symvers-$(KERN_VER_NO_OFED.gz) \
                    $(wildcard /boot/symvers-$(KERN_VER_NO_OFED) \
                    $(wildcard /usr/src/linux-headers-$(KERN_VER_NO_OFED)/Module.symvers))))

                $(eval $(call setup_kernel_ib_core_modules,kernels/$(KERN_VER_NO_OFED)/drivers/infiniband/core/Makefile,Kernel $(KERN_VER),$(KERN_SYMVERS)))
            else
                INFO_CORE_MOD = NOT Building IB Core Modules for $(KERN_VER)
            endif
        else
            # Unknown OFED
            $(error Unknown OFED $(OFED_VER_STRING))
        endif
    endif
endif


export OFED_VER_TYPE

ifeq ($(shell grep -w call_usermodehelper_setfns $(KSRC1)/include/linux/kmod.h 2> /dev/null),)
    cflags += -DKS_HAS_CALL_USERMODEHELPER_SETFNS=0
else
    cflags += -DKS_HAS_CALL_USERMODEHELPER_SETFNS=1
endif

ifneq ($(wildcard $(OFA_KERNEL)/compat/config.h),)
    INCLUDES = -include $(OFA_KERNEL)/compat/config.h
endif

ifneq ($(shell grep -w __tcp_send_ack $(KSRC1)/include/net/tcp.h 2> /dev/null),)
        cflags += -DKS_HAS___TCP_SEND_ACK=1
else
        cflags += -DKS_HAS___TCP_SEND_ACK=0
endif

ifneq ($(shell grep -w tcp_reno_undo_cwnd $(KSRC1)/include/net/tcp.h 2> /dev/null),)
        cflags += -DKS_HAS_TCP_RENO_UNDO_CWND=1
else
        cflags += -DKS_HAS_TCP_RENO_UNDO_CWND=0
endif

ifneq ($(shell grep -w tcp_reno_undo_cwnd $(KSRC1)/include/net/tcp.h 2> /dev/null),)
        cflags += -DKS_HAS_TCP_RENO_UNDO_CWND=1
else
        cflags += -DKS_HAS_TCP_RENO_UNDO_CWND=0
endif

ifneq ($(shell grep -w mmap_read_lock $(KSRC1)/include/linux/mmap_lock.h 2> /dev/null),)
        cflags += -DKS_HAS_MMAP_LOCK_FUNCTIONS=1
else
        cflags += -DKS_HAS_MMAP_LOCK_FUNCTIONS=0
endif

ifneq ($(shell grep -w mmap_write_trylock $(KSRC1)/include/linux/mmap_lock.h 2> /dev/null),)
        cflags += -DKS_HAS_MMAP_WRITE_TRYLOCK=1
else
        cflags += -DKS_HAS_MMAP_WRITE_TRYLOCK=0
endif

ifneq ($(shell grep -w revalidate_disk_size $(KSRC1)/include/linux/genhd.h 2> /dev/null),)
        cflags += -DKS_HAS_REVALIDATE_DISK_SIZE=1
else
        cflags += -DKS_HAS_REVALIDATE_DISK_SIZE=0
endif

ifneq ($(shell grep -w bio_start_io_acct $(KSRC1)/include/linux/blkdev.h 2> /dev/null),)
        cflags += -DKS_HAS_BIO_START_IO_ACCT=1
else
        cflags += -DKS_HAS_BIO_START_IO_ACCT=0
endif

# on newer kernel set_fs is there only if CONFIG_SET_FS is on
#ifneq ($(shell grep -w CONFIG_SET_FS $(KSRC1)/include/asm-generic/uaccess.h 2> /dev/null),)
        #cflags += -DKS_HAS_SET_FS=0
#else
        #cflags += -DKS_HAS_SET_FS=1
#endif

# Include common.mk
ifneq ($(M),)
    include $(M)/scripts/common.mk
    include $(M)/scripts/backports.mk
else
    include scripts/common.mk
    include scripts/backports.mk
endif

cflags += -Wall -Wstrict-prototypes
cflags += -Werror -Wno-error=unused-function -Wno-vla

ifeq ($(LLVM),)
GCC_VERCODE=$(shell gcc -dumpfullversion -dumpversion | sed -e 's/\.\([0-9][0-9]\)/\1/g' -e 's/\.\([0-9]\)/0\1/g' -e 's/^[0-9]\{3,4\}$$/&00/')
GCC800_VERCODE=80000
ifeq ($(shell test $(GCC_VERCODE) -gt $(GCC800_VERCODE); echo $$?),0)
	# Disable some GCC 8 and above warnings that cause issues
	cflags += -Wno-missing-attributes
endif

GCC1300_VERCODE=130000
ifeq ($(shell test $(GCC_VERCODE) -gt $(GCC1300_VERCODE); echo $$?),0)
	# Disable some GCC 13 and above warnings that cause issues
	cflags += -Wno-attribute-warning
endif
endif

# for KASAN
#cflags += -g -O1 -ggdb -fno-builtin
# debug
# cflags += -g -O1 -DCONFIG_NVMEIB_DEBUG -DDEBUG -DMGMT_STATS
# cflags += -DCONFIG_NVMEIB_DEBUG -DDEBUG
# cflags += -O3 -DTAKE_STATS -DMGMT_STATS
# cflags += -O0 -DMGMT_STATS
cflags += -DDEBUG_FIELDSIZE_OFERFLOW
cflags += -DDEBUG_LOCKS_CORRUPTION
# cflags += -DDEBUG_CONTENDED_LOCKS
# cflags += -DDEBUG_NVMEIB_Q

ifeq ($(MEM),low)
    cflags += -DLOW_MEM
endif

ifeq ($(DEBUG),yes)
    cflags += -g -O1 -DCONFIG_NVMEIB_DEBUG -DDEBUG -DMGMT_STATS
else
  #cflags += -O3 -DMGMT_STATS
  cflags += -g -O3
endif

ifeq ($(MEM_USAGE),yes)
# has some performance penalty
   cflags += -DNVMEIB_COUNT_MEM_USAGE
# has significant performance penalty
#   cflags += -DNVMEIB_COUNT_MEM_USAGE_BACKTRACE
endif

cflags += -DNVMEIB_COMPILATION_DATE=NVMEIB_DATE\($(shell date +%-d,%-m,%Y)\)

#
# Disable in (1) Final release version and (2) Performance testing
#
# cflags += -DDEBUG_SUM
# cflags   += -DNVMEIBS_CLIENTS_PROC
# cflags += -O3
# cflags += -fno-inline-small-functions -DDEBUG
# cflags += -g -O0 -fno-inline-small-functions
# cflags += -DDEBUG_OVEREAGER=0x3fff
# cflags += -DDEBUG_UNCOMPLETED
# cflags += -DTAKE_STATS
# cflags += -DBLKDEV_PROFILING
# cflags += -DDEBUG_TOPO_CNTRS -DAUTONOMOUS_SYNCS_STATS
cflags += -DDEBUG_TOMA_REG_LEAKS
# cflags += -DDEBUG_PERCPU_ISSUED_IO_CNTRS
#cflags += -DDEBUG_TRANSFERS
# cflags += -DDEBUG_TRANSFERS_DETECT_DBL_CB
# cflags += -DDEBUG_TRANSFERS_DETECT_FAIL_TO_UNMAP
# cflags += -DDEBUG_LOCK_RETRY
# cflags += -DDEBUG_CLNT_NET_STATS
# cflags += -DDEBUG_USING_RADIX=1
# cflags += -DDEBUG_SCQ_IU_OWNER=1 -DDEBUG_SCQ_IU_OWNER_BT
cflags += -DDEBUG_REQ_REUSED_BB_STATE=0

cflags += -DNVMEIBC_NRCH_DEFER_COMPLETE_IOCMD=1 #value must be set to 0/1
cflags += -DNVMEIBC_LOCAL_DEFER_COMPLETE_IOCMD=1 #value must be set to 0/1

cflags += -DNVMEIBC_DISK_CMDS_STATS=1
# uncomment below line to enable per volume disk statistics (LinkedIn)
cflags += -DNVMEIBC_ENABLE_PER_VOLUME_STATS=1
#cflags += -DNVMEIBC_DISK_CMDS_STATS_DBL_COMP_BT=1
#cflags += -DNVMEIBC_DISK_CMDS_STATS_PROBES=1
cflags += -DNVMEIB_QP_STATS=1
#cflags += -DNVMEIBC_DEBUG_FR_LEAK

#cflags += -DNVMEIB_STATE_GUARD_STACK_TRACE
cflags += -DNVMEIBC_LOCKS_CHANNEL_GUARD_STATE=0
#cflags += -DNVMEIBS_NR_CATCH_IO_DBL_CB
#cflags += -DNVMEIBS_NR_CATCH_CMD_ALREADY_UNDERWAY
cflags += -DNVMEIB_DEBUG_RDMA_CORRUPTION=0
#cflags += -DNVMEIBC_DISK_CMD_DEBUG_UNCOMPLETED=1

cflags += -DNVMEIBC_READ_POISON_BB=1
cflags += -DNVMEIBC_READ_POISON_BB_BYTES=256
cflags += -DNVMEIBC_READ_POISON_BB_PANIC=0

cflags += -DNVMEIBC_NR_LAT_MEAS=0
cflags += -DNVMEIBS_NR_LAT_MEAS=0

ifeq ($(SECTOR_SHIFT),)
    SECTOR_SHIFT_FLAG = NVMEIBC_SECTOR_SHIFT=12
else
    SECTOR_SHIFT_FLAG = NVMEIBC_SECTOR_SHIFT=$(SECTOR_SHIFT)
endif
cflags += -D$(SECTOR_SHIFT_FLAG)
UTILSFLAGS += -D$(SECTOR_SHIFT_FLAG)
UTILSFLAGS += -DUSER_SPACE

# polling on client side instead of receive message
# cflags += -DUSE_RDMA_POLLING
#cflags+=-DHAVE_TIMECOUNTER_H -DMLNX_OFED_3_1 -DHAVE_NETDEV_RSS_KEY_FILL -DOFED_VER_MAJ=3 -DOFED_VER_MIN=1
cflags += -DTRACE_CPUID
# cflags += -DSRQ_TRACE

ifneq ($(COMMIT_ID),)
    cflags +=-DCOMMIT_ID=0x$(COMMIT_ID)
    UTILSFLAGS += -DCOMMIT_ID=0x$(COMMIT_ID)
else
    cflags += -DCOMMIT_ID=0x0
endif


ifneq ($(BRANCH_NAME),)
    cflags += -DBRANCH_NAME=\"$(BRANCH_NAME)\"
else
    cflags += -DBRANCH_NAME="none"
endif

# When Including common files with Toma, notify that this is kernel compilation

ifeq ($(DISTRO_TAG),)
    DISTRO_TAG = $(shell $(NVMESH_SRC_DIR)/tools/distro_tag.sh)
endif

ifeq ($(PACKAGE_BUILD_NUMBER),)
    PACKAGE_BUILD_NUMBER = "buildnumber"
endif

cflags += -DNVMESH_VERSION=$(VERSION) -DNVMESH_RELEASE=$(RELEASE) -DBUILD_DISTRO=$(DISTRO_TAG) -DBUILD_NUMBER=$(PACKAGE_BUILD_NUMBER)

# Used for RDDA Check
cflags += -DKERN_VER_STRING=\""$(KERN_VER)\"" -DOFED_VER_STRING=\""$(OFED_VER_STRING)\"" -DINBOX_OFED_VER_STRING=\""$(INBOX_OFED_VER_STRING)\""

#Add poller thread to replace the softirq mechanism
cflags += -DIO_POLL_THREAD=1
# cflags += -DCQ_DEBUG=1

# Support older nvmeiba (without detaching atoms upgrade support) in nvmeibc
cflags += -DNVMEIBC_ATOM_MIGHT_NOT_SUPPORT_DETACHING=1

# EC PERFORMANCE (full-slice oriented)
cflags += -DEC_PERF_CLNT_NORDDA_REDUCE_SEND_COMPS=0
cflags += -DEC_PERF_CLNT_NORDDA_SHARED_CQ=1

# Developer Mode flags
ifeq ($(IS_DEVELOPMENT),yes)
    cflags += -DNVMESH_IS_PRODUCTION_COMPILATION=0
else ifeq ($(IS_DEVELOPMENT),no)
    cflags += -DNVMESH_IS_PRODUCTION_COMPILATION=1
    cflags += -DDBGDI_REMOVED_IN_PRODUCTION
else ifeq ($(IS_DEVELOPMENT),)
    # So we can control default value
    cflags += -DNVMESH_IS_PRODUCTION_COMPILATION=0
endif
#cflags += -DNVMEIB_DVLP_UNSAFE
cflags += -DNVMEIB_TRANSPORT_SKIP_STAGES
#cflags += -DNVMEIB_TRANSPORT_AUTOCOMP_IO

CFLAGS_NO_KERNEL_INCLUDES := $(cflags)
UTILSFLAGS += $(CFLAGS_NO_KERNEL_INCLUDES)
cflags += $(INCLUDES)

#
# Auto generated files
#

# Autogen dir's absolute path
export AUTOGEN_DIR = $(shell pwd)/autogen
export TOOLS_DIR = $(shell pwd)/tools
export SCRIPTS_DIR = $(shell pwd)/scripts
# Autogen dir's subdirs - generated by autogen/Makefile
export AUTOGEN_SUBDIRS = common clnt srv toma
# Autogen dir's subdirs included by kernel modules
AUTOGEN_SUBDIRS_KERN = common clnt srv
AUTOGEN_INCS := $(foreach dir,$(AUTOGEN_SUBDIRS_KERN),-I$(AUTOGEN_DIR)/$(dir))
# Autogen dir's subdirs included by Toma
export AUTOGEN_SUBDIRS_TOMA = common toma
# Autogen dir's compile & clean commands
COMPILE_LZ4 = +$(MAKE) -C $(TOOLS_DIR)/lz4 BUILD_SHARED=no BUILD_STATIC=yes lib-release
COMPILE_COMPRESS = +$(MAKE) -C $(TOOLS_DIR)/trace_compress_lib all
COMPILE_AUTOGEN = +$(MAKE) -C $(AUTOGEN_DIR) NVMESH_SRC_DIR=$(NVMESH_SRC_DIR) all
COMPILE_TRACE_DAEMON_2 = +$(MAKE) -C $(TOOLS_DIR)/trace_daemon_2.0 BUILD_DIR=$(NVMESH_SRC_DIR)
COMPILE_PIPE_TRACER = +$(MAKE) -C $(TOOLS_DIR)/pipe_tracer
COMPILE_PAGER = +$(MAKE) -C $(TOOLS_DIR)/traces_post_processor pager NVMESH_SRC_DIR=$(NVMESH_SRC_DIR)
COMPILE_FORMATTERS = +$(MAKE) -C $(TOOLS_DIR)/traces_post_processor/formatters SSDA=$(NVMESH_SRC_DIR)
COMPILE_SHARED_INFRA = +$(MAKE) -C $(TOOLS_DIR)/infra_shared SSDA=$(NVMESH_SRC_DIR)
COMPILE_NVME= +$(MAKE) -C $(SCRIPTS_DIR)/target/nvme-cli CFLAGS="-std=c99 -Wall"
COLLECT_DICTIONARIES = ./collect_dictionaries.sh
CLEAN_AUTOGEN = +$(MAKE) -C $(AUTOGEN_DIR) NVMESH_SRC_DIR=$(NVMESH_SRC_DIR) clean
CLEAN_LZ4 = +$(MAKE) -C $(TOOLS_DIR)/lz4 clean
CLEAN_COMPRESS = +$(MAKE) -C $(TOOLS_DIR)/trace_compress_lib clean
CLEAN_TRACE_DAEMON_2 = +$(MAKE) -C $(TOOLS_DIR)/trace_daemon_2.0 clean
CLEAN_TRACE_PP_DIR = find -name .trace_pp_dir | xargs rm -Rf
CLEAN_PIPE_TRACER = +$(MAKE) -C $(TOOLS_DIR)/pipe_tracer clean
CLEAN_PAGER = +$(MAKE) -C $(TOOLS_DIR)/traces_post_processor clean
CLEAN_FORMATTERS = +$(MAKE) -C $(TOOLS_DIR)/traces_post_processor/formatters SSDA=$(NVMESH_SRC_DIR)
CLEAN_DICTIONARIES = rm -f dictionaries.tar.gz
CREATE_VERSION = printf "version=\"$(VERSION)\"\ncommit=\"$(COMMIT_ID)\"\nbranch=\"$(BRANCH_NAME)\"" > version
CLEAN_VERSION = rm -f version
CLEAN_NVME = +$(MAKE) -C $(SCRIPTS_DIR)/target/nvme-cli clean

ifeq ($(BUILD_PAGER),no)
    COMPILE_PAGER =
endif

# Soft-iWARP
ifneq ($(BUILD_TCP),)
INFO_SIW := SoftiWARP (SIW)
# Not needed anymore because siw is not compiled separately
#SIW_SYMVERS := $(shell pwd)/softiwarp/kernel/Module.symvers
cflags += -DENABLE_SIW=1

#cflags += -DSIW_DEBUG_RX_CRC=1 -DSIW_DEBUG_TX_CRC=1 -DSIW_DEBUG_CQ=1 -DSIW_DEBUG_SRQ=1 -DSIW_TEST_RQE_RETRY=1
#cflags += -DSIW_DEBUG_RX_CRC=1 -DSIW_DEBUG_TX_CRC=1
obj-m += softiwarp/kernel/
else
cflags += -DENABLE_SIW=0
endif

subdir-ccflags-y += $(cflags)

# ofed_symbol_version
ifneq ($(OFED_SYM_VER),)
    OFED_SYMVERS = $(OFED_SYM_VER)
    INFO_OFED := Override - Mellanox OFED $(OFED_VER) in $(OFED_SRC_DIR). Symbols from $(OFED_SYMVERS)
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
    -I$$(srctree)/arch/$$(SRCARCH)/include/generated \
    -I$$(srctree)/arch/$$(SRCARCH)/include/generated/uapi \
    -I$$(srctree)/include/generated/uapi \
    -Iarch/$$(SRCARCH)/include/generated/uapi \
    -I$$(srctree)/include \
    -I$$(srctree)/include/uapi \
    -Iinclude/generated/uapi \
    $$(if $$(KBUILD_SRC),-Iinclude2 -I$$(srctree)/include) \
    -I$$(srctree)/arch/$$(SRCARCH)/include \
    -Iarch/$$(SRCARCH)/include/generated \
    -I$(shell pwd) -I$(shell pwd)/common -I$(shell pwd)/common_public -I$(shell pwd)/srv -I$(shell pwd)/clnt -I$(shell pwd)/toma\
    -I$(shell pwd)/softiwarp -I$(shell pwd)/softiwarp/common -I$(shell pwd)/utils/nvmeib_jdr \
    $(AUTOGEN_INCS) \
    $(INC_DIR2)'

#COMPILE_MODULES='\
#	$(VV)$(MAKE) -C $(KSRC) M=$(PWD) V=$(V) EXTRA_CFLAGS="$(cflags) $(EXTRA_CFLAGS) -save-temps=obj -D__FIRST_PASS__" BNXT_CFLAGS="$(BNXT_CFLAGS)" \
#		 LINUXINCLUDE=$(LINUX_INCLUDE) \
#		 KBUILD_EXTRA_SYMBOLS="$(OFED_SYMVERS) $(BNXT_SYMVERS) $(SIW_SYMVERS)" modules'
COMPILE_SYMVERS=\
	$(VV)$(MAKE) -C $(KSRC) M=$(PWD) V=$(V) EXTRA_CFLAGS='$(cflags) $(EXTRA_CFLAGS)' BNXT_CFLAGS="$(BNXT_CFLAGS)" \
	LINUXINCLUDE=$(LINUX_INCLUDE) \
	KBUILD_EXTRA_SYMBOLS="$(OFED_SYMVERS) $(BNXT_SYMVERS) $(SIW_SYMVERS)"
COMPILE_MODULES=\
	$(VV)$(MAKE) $(JOBS) -C $(KSRC) M=$(PWD) V=$(V) EXTRA_CFLAGS='$(cflags) $(EXTRA_CFLAGS)' BNXT_CFLAGS="$(BNXT_CFLAGS)" \
	LINUXINCLUDE=$(LINUX_INCLUDE) \
	KBUILD_EXTRA_SYMBOLS="$(OFED_SYMVERS) $(BNXT_SYMVERS) $(SIW_SYMVERS)" modules

all:
	$(info ============== Build Configuration ================)
	$(info CC: $(shell which $(CC)) - $(shell $(CC)  --version | head -1))
	$(info Branch: $(BRANCH_NAME) Commit: $(COMMIT_ID))
	$(info Kernel: $(KERN_VER) $(KSRC1))
	$(info OFED: Are we OFED? $(OFED_WE_R), $(INFO_OFED))
	$(info Other Drivers: $(INFO_BNXT) $(INFO_SIW))
	$(info $(INFO_TOMA))
	$(info $(INFO_SERV_CLNT))
	$(info $(INFO_TOMA))
	$(info $(INFO_RPM))
	$(info $(INFO_TEST))
	$(info $(INFO_CORE_MOD))
	$(info ===================================================)
	@$(call nconfig_save,$(configs))
	$(COMPILE_LZ4)
	$(COMPILE_COMPRESS)
	$(COMPILE_AUTOGEN)
	$(COMPILE_TRACE_DAEMON_2)
	$(COMPILE_PIPE_TRACER)
	$(COMPILE_PAGER)
	$(COMPILE_FORMATTERS)
	$(COMPILE_SHARED_INFRA)
	$(COMPILE_NVME)
	$(shell touch $(PWD)/clnt/block/datapath_ec/.nvmeibc_block_dp_ec_gf_asm.o.cmd)
ifeq ($(BUILD_KERNEL_MODULES),yes)
ifeq ($(IS_TOMA_FIRST),true)
	$(COMPILE_SYMVERS)
else
	$(COMPILE_MODULES)
endif
endif
ifeq ($(COMPILE_COMMON),yes)
    ifeq ($(MODVERSIONS), 1)
	+$(GEN_USED_SYMVERS) $(PWD)
	$(VV)cd $(PWD)/symvers && autoconf && ./configure --with-kern-ver=$(KERN_VER)
	$(VV)$(MAKE) -C $(KSRC) M=$(PWD)/symvers V=$(V) EXTRA_CFLAGS="$(cflags) $(EXTRA_CFLAGS)" BNXT_CFLAGS="$(BNXT_CFLAGS)" \
	LINUXINCLUDE=$(LINUX_INCLUDE) \
	KBUILD_EXTRA_SYMBOLS="$(OFED_SYMVERS) $(BNXT_SYMVERS) $(SIW_SYMVERS)" modules
    endif
endif
	+$(VV)$(CHECK_IB_CORE_MOD)
	+$(VV)$(ARC_IB_CORE_MOD)
	+$(COMPILE_TOOLS)
	+$(VV)$(COMPILE_TOMA) $(TOMA_LLVM) $(TOMA_SILENT)
ifeq ($(BUILD_KERNEL_MODULES),yes)
ifeq ($(IS_TOMA_FIRST),true)
	$(COMPILE_MODULES)
endif
endif
	+$(VV)$(COMPILE_UTILS)
	$(MAKE) -C $(TOOLS_DIR)/toma_rpc
	$(VV)$(COLLECT_DICTIONARIES)
	$(VV)$(COMPRESS_KERNEL_MODULES)
	$(info $(PY_TO_EXEC_INFO))
	$(VV)$(PY_TO_EXEC)
	$(VV)$(BUILD_RPM)
	$(VV)$(CREATE_VERSION)

.PHONY: clean install

clean:
	$(MAKE) -C $(KSRC) M=$(PWD) clean
	+$(CLEAN_DICTIONARIES)
	+$(CLEAN_TOMA)
	+$(CLEAN_TOOLS)
	+$(CLEAN_UTILS)
	+$(CLEAN_AUTOGEN)
	+$(CLEAN_LZ4)
	+$(CLEAN_COMPRESS)
	+$(CLEAN_TRACE_DAEMON_2)
	+$(CLEAN_PIPE_TRACER)
	+$(CLEAN_PAGER)
	+$(CLEAN_FORMATTERS)
	+$(CLEAN_VERSION)
	+$(CLEAN_NVME)
	+$(CLEAN_TRACE_PP_DIR)
.NOTPARALLEL:

MODULES_DIR := /lib/modules/$(KERN_VER)/extra/nvmesh
NVMESH_PREFIX_DIR := /opt/nvmesh
COMMON_REPO_DIR := $(NVMESH_PREFIX_DIR)/common-repo
CLIENT_REPO_DIR := $(NVMESH_PREFIX_DIR)/client-repo
TARGET_REPO_DIR := $(NVMESH_PREFIX_DIR)/target-repo
EXECUTABLES_DIR	:= $(NVMESH_PREFIX_DIR)/bin
DEST_TOOLS_DIR	:= $(NVMESH_PREFIX_DIR)/tools
DEST_MCS_DIR := $(CLIENT_REPO_DIR)/management_cm
DEST_COMMON_REPO_DIR := $(COMMON_REPO_DIR)/common_$(OFED_VER_STRING)_$(KERN_VER)
DEST_CLIENT_REPO_DIR := $(CLIENT_REPO_DIR)/client_$(OFED_VER_STRING)_$(KERN_VER)
DEST_TARGET_REPO_DIR := $(TARGET_REPO_DIR)/target_$(OFED_VER_STRING)_$(KERN_VER)
DEST_TOMA_DIR := $(DEST_TARGET_REPO_DIR)/toma
DEST_CONF_DIR := /etc/nvmesh
DEST_CONF_D_DIR := /etc/nvmesh/nvmesh.conf.d
VAR_OPT_DIR := /var/opt/nvmesh
MODPROBE_D_DIR := /etc/modprobe.d
DEPMOD_D_DIR := /etc/depmod.d
UDEV_RULES_D_DIR := /etc/udev/rules.d
DEST_LOG_DIR := /var/log/nvmesh
TRACE_DAEMON_DIR := /var/log/nvmesh/trace_daemon

ifeq ($(shell pgrep systemd 2> /dev/null | head -1),1)
    INSTALL_STARTUP_SCRIPTS_CMD := cp $(NVMESH_SRC_DIR)/system.d/*.service /usr/lib/systemd/system
else
    INSTALL_STARTUP_SCRIPTS_CMD := cp $(NVMESH_SRC_DIR)/init.d/nvmesh* /etc/init.d
endif

install_files:
	$(info Installing files to $(DEST_COMMON_REPO_DIR))
	@mkdir -p $(DEST_COMMON_REPO_DIR)
	@mkdir -p $(DEST_COMMON_REPO_DIR)/common
	$(info Installing scripts to $(COMMON_REPO_DIR)/common/scripts)
	@mkdir -p $(COMMON_REPO_DIR)/scripts
	@cp $(NVMESH_SRC_DIR)/scripts/common/* $(COMMON_REPO_DIR)/scripts
	$(info Installing tools to $(COMMON_REPO_DIR)/tools)
	@mkdir -p $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/dictionary.json $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/infra_shared/infra_shared.so $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/nvmesh_netlink.py $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/read_dwarf.py $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/nvmesh_memmgr_monitor.py $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/nvmesh_client_upgrade_breakdown.py $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/nvmesh_metrics.py $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/toma_rpc/toma_rpc $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/perfTest/io_stress/scan_locks/scan_locks_ec $(COMMON_REPO_DIR)/tools
	@mkdir -p $(COMMON_REPO_DIR)/tools/trace_daemon_2.0
	@cp $(NVMESH_SRC_DIR)/tools/trace_daemon_2.0/trace_daemon $(COMMON_REPO_DIR)/tools/trace_daemon_2.0
	@cp $(NVMESH_SRC_DIR)/tools/trace_daemon_2.0/run_trace_daemon.sh $(COMMON_REPO_DIR)/tools/trace_daemon_2.0
	@mkdir -p $(COMMON_REPO_DIR)/tools/pipe_tracer
	@cp $(NVMESH_SRC_DIR)/tools/pipe_tracer/build/bin/nvmeib_pipe_tracer $(COMMON_REPO_DIR)/tools/pipe_tracer
	@mkdir -p $(COMMON_REPO_DIR)/tools/traces_post_processor
	@cp $(NVMESH_SRC_DIR)/tools/traces_post_processor/cpager $(COMMON_REPO_DIR)/tools/traces_post_processor
	@cp $(NVMESH_SRC_DIR)/tools/traces_post_processor/pager.py $(COMMON_REPO_DIR)/tools/traces_post_processor
	$(info Installing files to $(DEST_COMMON_REPO_DIR)/common)
	@cp $(NVMESH_SRC_DIR)/common/*.ko $(DEST_COMMON_REPO_DIR)/common
	@cp $(NVMESH_SRC_DIR)/common/*.json $(DEST_COMMON_REPO_DIR)/common
	@cp $(NVMESH_SRC_DIR)/dictionaries.tar.gz $(DEST_COMMON_REPO_DIR)
	@mkdir -p $(DEST_COMMON_REPO_DIR)/common_public
	@cp -R $(NVMESH_SRC_DIR)/common_public $(DEST_COMMON_REPO_DIR)
	@mkdir -p $(DEST_COMMON_REPO_DIR)/keeper
	-@cp $(NVMESH_SRC_DIR)/keeper/*.ko $(DEST_COMMON_REPO_DIR)/keeper
	@mkdir -p $(DEST_COMMON_REPO_DIR)/softiwarp/kernel
	-@cp -R $(NVMESH_SRC_DIR)/softiwarp/kernel/*.ko $(DEST_COMMON_REPO_DIR)/softiwarp/kernel
	$(info Installing files to $(DEST_CLIENT_REPO_DIR))
	@mkdir -p $(DEST_CLIENT_REPO_DIR)/client/atom
	@cp $(NVMESH_SRC_DIR)/clnt/atom/*.ko $(DEST_CLIENT_REPO_DIR)/client/atom
	@cp $(NVMESH_SRC_DIR)/clnt/*.ko $(DEST_CLIENT_REPO_DIR)/client
	@cp $(NVMESH_SRC_DIR)/clnt/*.json $(DEST_CLIENT_REPO_DIR)/client
	@mkdir -p $(DEST_CLIENT_REPO_DIR)/symvers
	-@cp $(NVMESH_SRC_DIR)/symvers/*.ko $(DEST_CLIENT_REPO_DIR)/symvers
	$(info Installing files to $(DEST_TARGET_REPO_DIR))
	@mkdir -p $(DEST_TARGET_REPO_DIR)/target
	@cp $(NVMESH_SRC_DIR)/srv/*.ko $(DEST_TARGET_REPO_DIR)/target
	@cp $(NVMESH_SRC_DIR)/srv/*.json $(DEST_TARGET_REPO_DIR)/target
	$(info Creating symlinks in $(MODULES_DIR))
	@mkdir -p $(MODULES_DIR)
	@ln -nsf $(DEST_COMMON_REPO_DIR) $(MODULES_DIR)/common
	@ln -nsf $(DEST_CLIENT_REPO_DIR) $(MODULES_DIR)/client
	@ln -nsf $(DEST_TARGET_REPO_DIR) $(MODULES_DIR)/target
	$(info Running depmod)
	@depmod -a
	$(info Installing modprobe.d files)
	@cp $(NVMESH_SRC_DIR)/modprobe.d/nvmesh.conf $(MODPROBE_D_DIR)
	@mkdir -p $(COMMON_REPO_DIR)/modprobe.d
	@cp $(NVMESH_SRC_DIR)/modprobe.d/* $(COMMON_REPO_DIR)/modprobe.d
	$(info Installing depmod.d files)
	@cp $(NVMESH_SRC_DIR)/depmod.d/* $(DEPMOD_D_DIR)
	$(info Installing udev rules.d files)
	@cp $(NVMESH_SRC_DIR)/rules.d/* $(UDEV_RULES_D_DIR)
	$(info Installing files to $(DEST_TOMA_DIR))
	@mkdir -p $(DEST_TOMA_DIR)/bin
	-@cp -R $(NVMESH_SRC_DIR)/toma/bin/debug $(DEST_TOMA_DIR)/bin
	-@cp -R $(NVMESH_SRC_DIR)/toma/bin/delease $(DEST_TOMA_DIR)/bin
	-@cp -R $(NVMESH_SRC_DIR)/toma/bin/release $(DEST_TOMA_DIR)/bin
	@cp -R $(NVMESH_SRC_DIR)/toma/utils $(DEST_TOMA_DIR)
	$(info Installing scripts to $(CLIENT_REPO_DIR)/scripts)
	@mkdir -p $(CLIENT_REPO_DIR)/scripts
	@cp $(NVMESH_SRC_DIR)/scripts/client/* $(CLIENT_REPO_DIR)/scripts
	$(info Installing scripts to $(CLIENT_REPO_DIR)/services)
	@mkdir -p $(CLIENT_REPO_DIR)/services
	@cp $(NVMESH_SRC_DIR)/init.d/nvmesh_util $(CLIENT_REPO_DIR)/services
	@cp $(NVMESH_SRC_DIR)/init.d/nvmeshclient $(CLIENT_REPO_DIR)/services
	$(info Installing scripts to $(TARGET_REPO_DIR)/services)
	@mkdir -p $(TARGET_REPO_DIR)/services
	@cp $(NVMESH_SRC_DIR)/init.d/nvmesh_util $(TARGET_REPO_DIR)/services
	@cp $(NVMESH_SRC_DIR)/init.d/nvmeshtarget $(TARGET_REPO_DIR)/services
	$(info Installing scripts to $(TARGET_REPO_DIR)/scripts)
	@mkdir -p $(TARGET_REPO_DIR)/scripts
	@cp -R $(NVMESH_SRC_DIR)/scripts/target/* $(TARGET_REPO_DIR)/scripts
	$(info Installing startup scripts)
	@$(INSTALL_STARTUP_SCRIPTS_CMD)
	$(info Installing binaries to /usr/bin)
	@cp -R $(NVMESH_SRC_DIR)/bin/* /usr/bin
	$(info Installing management_cm to $(DEST_MCS_DIR))
	@mkdir -p $(DEST_MCS_DIR)
	@cp -R $(NVMESH_SRC_DIR)/management_cm/* $(DEST_MCS_DIR)
	$(info Creating version files)
	@cp version $(CLIENT_REPO_DIR)/version
	@cp version $(TARGET_REPO_DIR)/version
	$(info Creating folders in $(VAR_OPT_DIR))
	$(info Installing tools to $(DEST_TOOLS_DIR))
	@mkdir -p $(DEST_TOOLS_DIR)
	@cp -R $(NVMESH_SRC_DIR)/tools/* $(DEST_TOOLS_DIR)
	$(info Creating directories in $(VAR_OPT_DIR))
	@mkdir -p $(VAR_OPT_DIR)/metadata_disk_image
	@mkdir -p $(VAR_OPT_DIR)/mcs
	@mkdir -p $(VAR_OPT_DIR)/toma
	@mkdir -p $(VAR_OPT_DIR)/block_devices_configuration
	@mkdir -p $(VAR_OPT_DIR)/block_devices_sub_vols
	@mkdir -p $(VAR_OPT_DIR)/clnt_instance_configuration
	$(info Installing tracing to $(TRACE_DAEMON_DIR))
	@mkdir -p $(TRACE_DAEMON_DIR)
	@ln -sf $(COMMON_REPO_DIR)/tools/traces_post_processor/cpager $(TRACE_DAEMON_DIR)/cpager
	@ln -sf $(COMMON_REPO_DIR)/tools/traces_post_processor/pager.py $(TRACE_DAEMON_DIR)/pager.py
	@tar zxvf $(NVMESH_SRC_DIR)/dictionaries.tar.gz -C $(TRACE_DAEMON_DIR)
	$(info Creating $(DEST_CONF_DIR))
	@mkdir -p $(DEST_CONF_DIR)
	$(info Creating $(DEST_CONF_D_DIR))
	@mkdir -p $(DEST_CONF_D_DIR)
.NOTPARALLEL:

$(DEST_CONF_DIR)/nvmesh.conf:
	$(info Creating nvmesh.conf)
	@cp $(NVMESH_SRC_DIR)/config/nvmesh.conf $(DEST_CONF_DIR)
	@read -p "Enter Management Protocol (http/https): " mgmt_prot;
	@sed -i "s/^MANAGEMENT_PROTOCOL=.*/MANAGEMENT_PROTOCOL=\"$$mgmt_prot\"/" /etc/nvmesh/nvmesh.conf
	@read -p "Enter Management Servers (e.g. nvmesh-management:4001): " mgmt_srvs;
	@sed -i "s/^MANAGEMENT_SERVERS=.*/MANAGEMENT_SERVERS=\"$$mgmt_srvs\"/" /etc/nvmesh/nvmesh.conf
	@read -p "Enter Kafka Servers (e.g. nvmesh-kafka:9092): " kafka_srvs;
	@sed -i "s/^KAFKA_SERVERS=.*/KAFKA_SERVERS=\"$$kafka_srvs\"/" /etc/nvmesh/nvmesh.conf
	@read -p "Enter NICs (e.g. mlx5_0:1): " nics;
	@sed -i "s/^CONFIGURED_NICS=.*/CONFIGURED_NICS=\"$$nics\"/" /etc/nvmesh/nvmesh.conf
.NOTPARALLEL:

$(DEST_CONF_DIR)/target_devices.conf:
	@cp $(NVMESH_SRC_DIR)/config/target_devices.conf $(DEST_CONF_DIR)

install: install_files $(DEST_CONF_DIR)/nvmesh.conf $(DEST_CONF_DIR)/target_devices.conf
.NOTPARALLEL:
