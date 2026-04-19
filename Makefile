# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# Parent Makefile
#

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

PY_TO_EXEC_VER ?= 3.10
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

# Kernel/RDMA compatibility -D flags for sched/mm, hashtable, genhd, ib_sa, etc.:
# see scripts/compute_backports.sh ("Moved from top-level Makefile") and scripts/backports.mk

# Broadcom Netxtreme Support not supported anymore, disabled by default
configs+=$(call nconfig_unset,BNXT)
cflags += -DBNXT_RE=0

ifeq ($(M),)
    NVMESH_SRC_DIR := $(shell pwd)
else
    NVMESH_SRC_DIR := $(M)
endif
export NVMESH_SRC_DIR

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

# OFED vs inbox RDMA paths, include dirs, and IB core module setup.
include $(NVMESH_SRC_DIR)/scripts/ofed_or_inbox.mk

# KS_HAS_CALL_USERMODEHELPER_SETFNS, KS_HAS___TCP_SEND_ACK, KS_HAS_TCP_RENO_UNDO_CWND,
# KS_HAS_MMAP_LOCK_*, KS_HAS_REVALIDATE_DISK_SIZE, KS_HAS_BIO_START_IO_ACCT: see scripts/compute_backports.sh

# Include common.mk (paths via NVMESH_SRC_DIR, same as M vs pwd above)
include $(NVMESH_SRC_DIR)/scripts/common.mk
include $(NVMESH_SRC_DIR)/scripts/backports.mk

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
    cflags +=-DCOMMIT_ID=0x$(COMMIT_ID) -DCOMMIT_ID_STR=\"$(COMMIT_ID)\"
    UTILSFLAGS += -DCOMMIT_ID=0x$(COMMIT_ID) -DCOMMIT_ID_STR=\"$(COMMIT_ID)\"
else
    cflags += -DCOMMIT_ID=0x0 -DCOMMIT_ID_STR=\"0\"
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
export AUTOGEN_DIR = $(NVMESH_SRC_DIR)/autogen
export TOOLS_DIR = $(NVMESH_SRC_DIR)/tools
export PET_DIR = $(NVMESH_SRC_DIR)/common/pet
export SCRIPTS_DIR = $(NVMESH_SRC_DIR)/scripts
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
COMPILE_TRACE_DAEMON_2 = +$(MAKE) -C $(TOOLS_DIR)/trace_daemon_2.0 BUILD_DIR=$(NVMESH_SRC_DIR) COMMIT_ID=0x$(COMMIT_ID) COMMIT_ID_STR=$(COMMIT_ID)
COMPILE_PIPE_TRACER = +$(MAKE) -C $(TOOLS_DIR)/pipe_tracer
COMPILE_PAGER = +$(MAKE) -C $(TOOLS_DIR)/traces_post_processor pager NVMESH_SRC_DIR=$(NVMESH_SRC_DIR)
COMPILE_FORMATTERS = +$(MAKE) -C $(TOOLS_DIR)/traces_post_processor/formatters SSDA=$(NVMESH_SRC_DIR)
COMPILE_SHARED_INFRA = +$(MAKE) -C $(TOOLS_DIR)/infra_shared SSDA=$(NVMESH_SRC_DIR)
COMPILE_NVME= +$(MAKE) -C $(SCRIPTS_DIR)/target/nvme-cli CFLAGS="-std=c99 -Wall"
COMPILE_PET = +$(MAKE) -C $(PET_DIR) fast_build COMMIT_ID=0x$(COMMIT_ID) COMMIT_ID_STR=$(COMMIT_ID) PY=$(PY_TO_EXEC_VER)
# Pass COMMIT_ID so tarball content is under <commit>/ for dictionary binding to log files.
# To disable PET dictionary build, set PET_MODULE to empty or undefined (PET_MODULE=$(PET_MODULE)). To enable, set it to the module default location (PET_MODULE=clnt/nvmeibc.ko), otherwise PET_MODULE=<path/to/nvmeibc.ko> if client module is built elsewhere.
PET_MODULE?=clnt/nvmeibc.ko
COLLECT_DICTIONARIES = COMMIT_ID=0x$(COMMIT_ID) PET_MODULE=$(PET_MODULE) ./collect_dictionaries.sh
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
#SIW_SYMVERS := $(NVMESH_SRC_DIR)/softiwarp/kernel/Module.symvers
cflags += -DENABLE_SIW=1

#cflags += -DSIW_DEBUG_RX_CRC=1 -DSIW_DEBUG_TX_CRC=1 -DSIW_DEBUG_CQ=1 -DSIW_DEBUG_SRQ=1 -DSIW_TEST_RQE_RETRY=1
#cflags += -DSIW_DEBUG_RX_CRC=1 -DSIW_DEBUG_TX_CRC=1
obj-m += softiwarp/kernel/
else
cflags += -DENABLE_SIW=0
endif

# EXTRA_CFLAGS was removed from kbuild in newer kernels (6.x+).
# Use subdir-ccflags-y so the flags propagate to all subdirectory builds.
ifeq ($(BACKPORTS_CFLAGS),)
    BACKPORTS_CFLAGS = $(backports_cflags)
endif

# User-space UTILSFLAGS snapshot (see CFLAGS_NO_KERNEL_INCLUDES) predates ENABLE_SIW and
# backports; append kernel-compat -D flags from compute_backports.sh for parity with modules.
UTILSFLAGS += $(BACKPORTS_CFLAGS)

subdir-ccflags-y += $(EXTRA_CFLAGS)

# if V = 1 we get a full trace of the compile command
V ?= 0

LINUX_INCLUDE='\
    $(INC_DIR) \
    -include $(autoconf_h) \
    $(KCONFIG_H) \
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
    -I$$(srctree)/arch/$$(SRCARCH)/include \
    -Iarch/$$(SRCARCH)/include/generated \
    -I$(NVMESH_SRC_DIR) -I$(NVMESH_SRC_DIR)/common -I$(NVMESH_SRC_DIR)/common_public -I$(NVMESH_SRC_DIR)/srv -I$(NVMESH_SRC_DIR)/clnt -I$(NVMESH_SRC_DIR)/toma\
    -I$(NVMESH_SRC_DIR)/softiwarp -I$(NVMESH_SRC_DIR)/softiwarp/common -I$(NVMESH_SRC_DIR)/utils/nvmeib_jdr \
    $(AUTOGEN_INCS) \
    $(INC_DIR2)'

#COMPILE_MODULES='\
#	$(VV)$(MAKE) -C $(KSRC) M=$(PWD) V=$(V) EXTRA_CFLAGS="$(cflags) $(EXTRA_CFLAGS) -save-temps=obj -D__FIRST_PASS__" BNXT_CFLAGS="$(BNXT_CFLAGS)" \
#		 LINUXINCLUDE=$(LINUX_INCLUDE) \
#		 KBUILD_EXTRA_SYMBOLS="$(OFED_SYMVERS) $(BNXT_SYMVERS) $(SIW_SYMVERS)" modules'
COMPILE_SYMVERS=\
	$(VV)$(MAKE) -C $(KSRC) M=$(PWD) V=$(V) EXTRA_CFLAGS='$(cflags) $(BACKPORTS_CFLAGS) $(EXTRA_CFLAGS)' BNXT_CFLAGS="$(BNXT_CFLAGS)" \
	LINUXINCLUDE=$(LINUX_INCLUDE) \
	KBUILD_EXTRA_SYMBOLS="$(OFED_SYMVERS) $(BNXT_SYMVERS) $(SIW_SYMVERS)"
COMPILE_MODULES=\
	$(VV)$(MAKE) $(JOBS) -C $(KSRC) M=$(PWD) V=$(V) EXTRA_CFLAGS='$(cflags) $(BACKPORTS_CFLAGS) $(EXTRA_CFLAGS)' BNXT_CFLAGS="$(BNXT_CFLAGS)" \
	LINUXINCLUDE=$(LINUX_INCLUDE) \
	KBUILD_EXTRA_SYMBOLS="$(OFED_SYMVERS) $(BNXT_SYMVERS) $(SIW_SYMVERS)" modules

COMPILE_CMDS_JSON=\
    -$(VV)$(NVMESH_SRC_DIR)/gen_compile_commands.py -d $(NVMESH_SRC_DIR) -o $(NVMESH_SRC_DIR)/compile_commands.json -r $(KSRC)

all:
	$(info ============== Build Configuration ================)
	$(info CC: $(shell which $(CC)) - $(shell $(CC)  --version | head -1))
	$(info Branch: $(BRANCH_NAME) Commit: $(COMMIT_ID))
	$(info Kernel: $(KERN_VER) $(KSRC1))
	$(info OFED: Are we OFED? $(OFED_WE_R), $(INFO_OFED))
	$(info Other Drivers: $(INFO_BNXT) $(INFO_SIW))
	$(info Logging backports.mk to $(GREP_DEBUG_LOGFILE))
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
	$(VV)$(MAKE) -C $(KSRC) M=$(PWD)/symvers V=$(V) EXTRA_CFLAGS="$(cflags) $(backports_cflags) $(EXTRA_CFLAGS)" BNXT_CFLAGS="$(BNXT_CFLAGS)" \
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
	+$(VV)$(COMPILE_CMDS_JSON)
	+$(VV)$(COMPILE_UTILS)
	$(MAKE) -C $(TOOLS_DIR)/toma_rpc
	$(VV)$(COMPILE_PET)
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

include $(NVMESH_SRC_DIR)/scripts/install.mk
