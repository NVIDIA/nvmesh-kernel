# ********** Define compiler and compilation flags *********
BUILD_RELEASE?=1
UNAME := $(shell uname)# Which operating system we use?
HT = $(shell uname -m)
USE_ARM = 0
CFLAGS = $(EXTRA_CFLAGS)
ifeq ($(UNAME),Darwin)
	USE_DARWIN=1
	CFLAGS += -m64
	LIBS_PATH = -L/usr/lib
else ifeq ($(HT),x86_64)
	USE_64_BITS=1
	CFLAGS += -m64
	LIBS_PATH = -L/usr/lib64
else ifeq ($(HT),aarch64)
	USE_ARM=1
	CFLAGS += -march=armv8.1-a+crc+fp+simd
	LIBS_PATH = -L/usr/lib64
else
	$(error Unknown OS/Host)
endif

# ********** Included directories of H files *********
# Main project directory. (relative to this Makefile)
THIS_MKFILE_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
NVMESH_ROOT_DIR_RLTV := $(THIS_MKFILE_DIR)/../../..
NVMESH_ROOT_DIR := $(realpath $(NVMESH_ROOT_DIR_RLTV))
SSDA = $(NVMESH_ROOT_DIR)
INCLUDES :=
INCLUDES += -I$(SSDA)/clnt/block/um_integration/ -I$(SSDA)/clnt/ -I$(SSDA)/srv/ -I$(SSDA)/common/ -I$(SSDA)/common_public/ -I$(SSDA)/toma/ -I$(SSDA)/autogen/common/ -I$(SSDA)/autogen/clnt/ -I$(SSDA)
INCLUDES += -I/usr/include

# ********** *.cpp sources *********
SOURCES :=
SOURCES += $(THIS_MKFILE_DIR)/nvmesh_dp_lib_api.c

# Block unitest simulator, not needed, because UM supplies the API's of transport
CPP_INLCUDE_SIMU = 0
ifeq ($(CPP_INLCUDE_SIMU),1)
	CDIR = $(SSDA)/clnt/block/unitest
	SOURCES += $(CDIR)/nvmeibc_simu_disk.c $(CDIR)/ib_incs.c $(CDIR)/nvmeib_common_all.c $(CDIR)/nvmesh_sim.c $(CDIR)/di_tracker.c
	SOURCES += $(CDIR)/mgmt/nvmeibm_conf_db.c $(CDIR)/mgmt/nvmeibm_mgmt_simu.c $(CDIR)/mgmt/nvmeibm_mcs_simu.c
	SOURCES += $(CDIR)/toma/nvmeibt_topology_simu.c $(CDIR)/toma/nvmeibt_toma_simu.c $(CDIR)/toma/nvmeibr_disk_metadata_sim.c
	SOURCES += $(CDIR)/server/nvmeibs_ssd_disk_sim.c $(CDIR)/server/nvmeibs_nic_dma_atomics.c $(CDIR)/server/nvmeibs_main_sim.c $(CDIR)/server/nvmeibs_serjio_deps_sim.c
endif

# /common, /common_public, not needed, provided by UM code
CPP_INLCUDE_COMMON = 0
ifeq ($(CPP_INLCUDE_COMMON),1)
	CDIR = $(SSDA)/common
	SOURCES += $(CDIR)/nvmeib_io_stats.c $(CDIR)/nvmeib_mcs.c $(CDIR)/nvmeib_kth_events.c $(CDIR)/../common_public/nvmeib_public_procfs.c
	CPDIR= $(SSDA)/common_public/kth
	SOURCES += $(CPDIR)/nvmeib_public_kth.c
endif

CBDIR = $(SSDA)/clnt/block
# os api, not needed, irrelevant for UM
CPP_INLCUDE_OS_API = 0
ifeq ($(CPP_INLCUDE_OS_API),1)
	SOURCES += $(CBDIR)/nvmeibc_block_api_os.c
	ADIR = $(SSDA)/clnt/atom
	SOURCES += $(ADIR)/nvmeiba_infra.c $(ADIR)/nvmeiba_atom_os_api.c $(ADIR)/nvmeiba_main.c
endif

# Kernel sources that are included in sync library
CPP_INLCUDE_BLOCK = 1
ifeq ($(CPP_INLCUDE_BLOCK),1)
	SOURCES += $(CBDIR)/datapath_ec/recov/nvmeibc_block_dp_ec_sync_txid_wraparound.c
	SOURCES += $(CBDIR)/datapath_mirror/nvmeibc_block_dp_mirror_sync.c
	SOURCES += $(CBDIR)/recovery/nvmeibc_block_dp_sync_common.c
	SOURCES += $(CBDIR)/recovery/nvmeibc_block_dp_sync_api_manager_flow_cntrs.c
	SOURCES += $(CBDIR)/recovery/nvmeibc_block_dp_sync_no_write_hole.c
	SOURCES += $(CBDIR)/datapath_ec/nvmeibc_block_dp_ec.c
	SOURCES += $(CBDIR)/datapath_ec/nvmeibc_block_dp_ec_gf.c
	SOURCES += $(CBDIR)/datapath_ec/nvmeibc_block_dp_ec_journal.c
	SOURCES += $(CBDIR)/datapath_ec/nvmeibc_block_dp_ec_reed_solomon.c
	SOURCES += $(CBDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_praid.c
	SOURCES += $(CBDIR)/datapath_ec/recov/nvmeibc_block_dp_ec_recov_cold_sync.c
	SOURCES += $(CBDIR)/datapath_ec/recov/nvmeibc_block_dp_ec_recov_hot.c
	SOURCES += $(CBDIR)/datapath_ec/recov/nvmeibc_block_dp_ec_recov_maintenance.c
	SOURCES += $(CBDIR)/datapath_ec/recov/nvmeibc_block_dp_ec_recovery_common.c
	SOURCES += $(CBDIR)/datapath_mirror/nvmeibc_block_dp_mirror.c
	SOURCES += $(CBDIR)/datapath_mirror/nvmeibc_block_dp_mirror_trim.c
	SOURCES += $(CBDIR)/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_buffers.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_cmd_lock_link.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_common.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_cpu_masks.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_lock_server.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_dbits.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_locks_scheme.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_block_md_tables.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_block_md.c
	SOURCES += $(CBDIR)/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.c
	SOURCES += $(CBDIR)/datapath_utils_generic/profiling/nvmeibc_block_dp_profiling_generic.c
	SOURCES += $(CBDIR)/controlpath/nvmeibc_b_cp_topo_algorithms.c
endif

ifeq ($(USE_ARM), 1)
	SOURCES += $(CBDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_arm_um.c
	SOURCES += $(CBDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_arm_neon4.c
	SOURCES += $(CBDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_arm_recov_neon.c
	ASM_SRC  = $(CBDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_asm_arm.S_shipped
else
	ASM_SRC  = $(CBDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_asm.S_shipped
endif

# Kernel sources that are included in good path io library
ifeq ($(CPP_INLCUDE_BLOCK),1)
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_operation.c
endif

# Datapath: retries are not needed (implemented by UM), only syncs are included in the library
CPP_INLCUDE_DATAPATH = 0
ifeq ($(CPP_INLCUDE_DATAPATH),1)
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_io_resubmitter.c
endif

# Recoveries run by servers with possible UM client
CPP_INLCUDE_RECOVERIES = 1
ifeq ($(CPP_INLCUDE_RECOVERIES),1)
	SOURCES += $(CBDIR)/recovery/nvmeibc_raid_recovery.c
	SOURCES += $(CBDIR)/recovery/nvmeibc_raid_recov_itr.c
	SOURCES += $(CBDIR)/datapath_ec/recov/nvmeibc_block_dp_ec_recov_cold.c
endif

CPP_INCLUDE_CONTROL_PATH = 0
ifeq ($(CPP_INCLUDE_CONTROL_PATH),1)
	SOURCES += $(CBDIR)/recovery/nvmeibc_decentralized_unreg.c
endif

# Control path: Configuration / Topology, Not needed as UM manages config/topology.
CPP_INLCUDE_CONFIG = 0
ifeq ($(CPP_INLCUDE_CONFIG),1)
	SDIR = $(SSDA)/clnt
	SOURCES += $(SDIR)/management_utils_common/nvmeibc_management_capi_parse_conf.c
	SOURCES += $(SDIR)/management_utils_common/nvmeibc_management_volume_conf_checks.c
	SOURCES += $(SDIR)/module/instance/nvmeibc_cinst.c
	SOURCES += $(SDIR)/nvmeibc_volume.c
	SOURCES += $(SDIR)/nvmeibc_main.c
	SOURCES += $(SDIR)/nvmeibc_common.c
	SOURCES += $(SDIR)/nvmeibc_cc_api.c
	SOURCES += $(SDIR)/nvmeibc_io_pet.c
	SOURCES += $(SDIR)/nvmeibc_targets.c
	SOURCES += $(SDIR)/nvmeibc_block.c
	SOURCES += $(CBDIR)/nvmeibc_block_api_conf.c
	SOURCES += $(CBDIR)/nvmeibc_block_api_nvmeshioctl.c
	SOURCES += $(CBDIR)/datapath_utils_generic/nvmeibc_block_dp_io_perm_alert.c
	SOURCES += $(CBDIR)/derived_apis/nvmeibc_block_api_d_carrier.c
	SOURCES += $(CBDIR)/controlpath/nvmeibc_b_cp_lost_srv_resources.c
	SOURCES += $(CBDIR)/controlpath/nvmeibc_b_cp_send_toma_msg.c
	SOURCES += $(CBDIR)/controlpath/nvmeibc_b_cp_topo_on_free.c
	SOURCES += $(CBDIR)/controlpath/nvmeibc_b_cp_topo_seg_on_active.c
	SOURCES += $(CBDIR)/controlpath/nvmeibc_b_cp_trs_hash.c
	SOURCES += $(CBDIR)/nvmeibc_topology.c
	TBDIR = $(SSDA)/toma/
	SOURCES += $(TBDIR)/nvmeibt_uuid.c
	SOURCES += $(TBDIR)/clnt/nvmeibt_client_protocol.c
endif

# Production code: utilities, irrelevant for UM
CPP_INLCUDE_UTILITIES = 0
ifeq ($(CPP_INLCUDE_UTILITIES),1)
	SOURCES += .$(SSDA)/perfTest/io_stress/cmp_blocks/cmp_blocks_impl.c
	SOURCES += $(CBDIR)/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_log.c
endif

# ********** All objects will reside in separate directory **********
OBJ_DIR_ROOT=99bin
ifeq ($(BUILD_RELEASE),1)
	OBJ_DIR=./$(OBJ_DIR_ROOT)/release
else
	OBJ_DIR=./$(OBJ_DIR_ROOT)/debug
endif

OBJECTS_ALL = $(patsubst %.c, $(OBJ_DIR)/%.o, $(SOURCES))
OBJECTS_UNITEST += $(OBJ_DIR)/$(THIS_MKFILE_DIR)/test.o # Unitest code for library

# Include dependency files if they exist, causes .c files to depend on their .h files
DEPEND_FILES_ALL = $(OBJECTS_ALL:.o=.d) $(OBJECTS_UNITEST:.o=.d)
DFLAGS = -MP -MMD

ASM_OBJ = $(patsubst %.S_shipped, $(OBJ_DIR)/%.o, $(ASM_SRC))
OBJECTS = $(OBJECTS_ALL)
OBJECTS += $(ASM_OBJ)

BACKUPS = $(SOURCES:.c=.~) $(ASM_OBJ:.o=.~)

# Compilation output (Lib/Exe)
LIB_NAME_OUT=nvmesh_dp_lib

# ********** Extract git information *********
ifeq ($(COMPILATION_DATE),)
	COMPILATION_DATE := BuildDate\($(shell date +%-d/%b/%Y:"%T")\)\["$(shell uname -sri)"\]
endif
ifeq ($(HAS_LOCAL_GIT),)
	HAS_LOCAL_GIT=$(shell git rev-parse --is-inside-work-tree)
endif
ifeq ($(HAS_LOCAL_GIT),true)
	COMMIT_ID=$(shell git log -n1 --format=%h)
	VER_TAGID=$(shell git describe --abbrev=0)
	BRANCH_NAME=$(shell git symbolic-ref 2>/dev/null --short --quiet HEAD || git rev-parse 2>/dev/null --abbrev-ref HEAD)
	NVMESH_RELEASE=$(shell git describe | cut -c 2- | head -c9)
endif
# Dummy commit info
ifeq ($(COMMIT_ID),)
	COMMIT_ID := 010101010
	VER_TAGID := v3.1.0
	BRANCH_NAME := Unknown
	NVMESH_RELEASE :=3.1.0-774
endif

# ********** Define compiler and compilation flags *********
CFLAGS += -Wall -Werror -std=gnu99 -Wdeclaration-after-statement -Wstrict-prototypes
CFLAGS += -Wextra -Wshadow -Werror=strict-aliasing -Wno-type-limits -falign-functions=8 -fvisibility=hidden
CFLAGS += -Wno-unused-variable -Wno-unused-parameter
CFLAGS += -DNVMESH_IS_PRODUCTION_COMPILATION=1 -DUSER_SPACE -DDP_LIB=1 -DDBGDI_REMOVED_IN_PRODUCTION -DAUTONOMOUS_SYNCS_STATS
CFLAGS += -DCOMMIT_ID=0x$(COMMIT_ID)ULL -DVER_TAGID=$(VER_TAGID) -DBRANCH_NAME=$(BRANCH_NAME)
CFLAGS += -DNVMESH_VERSION=$(VER_TAGID) -DNVMESH_RELEASE=$(NVMESH_RELEASE) -DBUILD_NUMBER=0.0
CFLAGS += -DNVMEIB_COMPILATION_DATE=${COMPILATION_DATE} -DNVMEIBC_SECTOR_SHIFT=12 -DDISABLE_ALL_TRACING

GCC_VERSION := $(shell gcc -dumpversion)
GCC_VERSION_FULL := $(shell gcc -dumpfullversion)
ADDRES_OF_PACKED_MIN_GCC_VERSION_CHECK := $(shell expr `printf '%s\n' "$(GCC_VERSION)" "9" | sort -V | head -n1` == "9")
ifeq "$(ADDRES_OF_PACKED_MIN_GCC_VERSION_CHECK)" "1"
	CFLAGS += -Wno-address-of-packed-member
endif

CC=gcc
USE_CLANG =0
ifeq ($(USE_CLANG),1)
	CFLAGS += -Wno-address-of-packed-member -Wno-unknown-attributes -Wno-unused-function -Wno-missing-braces
	CC=clang
endif
ifeq ($(BUILD_RELEASE),1)
	CFLAGS += -O3 -g -DNDEBUG
else
	CFLAGS += -g3 -ggdb3
endif

# ********** Define Linker flags *********
LDFLAGS = -rdynamic -no-pie -ldl -lpthread -luuid

# ********** Actions *********
define print_compilation_info
	@printf "\t* Compilation info, Release=$(BUILD_RELEASE), COMMIT_ID=$(COMMIT_ID), $(CC)=$(GCC_VERSION_FULL), ARM=$(USE_ARM) HT=$(HT)|\n\n"
	@printf "\t* CFLAGS  | $(CFLAGS)\n"
	@printf "\t* INCLUDS | $(INCLUDES)\n"
	@printf "\t* DFLAGS  | $(DFLAGS)\n"
	@printf "\t* LDLAGS  | $(LDFLAGS)\n"
	@printf "===========================================\n"
	@printf "\t* Objects | $(shell echo "$(OBJECTS)" | sed -e 's/ /\t/g')\n"
	@printf "\t* Unitest | $(OBJECTS_UNITEST)\n"
	@printf "===========================================\n"
	@printf "\t* ASM_src | $(ASM_SRC)\n"
	@printf "\t* ASM_obj | $(ASM_OBJ)\n"
	@printf "===========================================\n"
endef

help:
	clear;
	@printf "Building |\e[0;32m$(LIB_NAME_OUT)\e[0;0m| for NVMesh-UM integration\n"
	$(call print_compilation_info);
	@printf "Usage |\e[0;32mmake all\e[0;0m| for building a lib + executable test\n"
	@printf "\n\n\n\n"

all: $(SOURCES) $(ASM_SRC) $(LIB_NAME_OUT).a $(LIB_NAME_OUT)
	$(info +--->Done!)

#rebuild: clean all

define print_building_target
	@printf "+--->Building |\e[0;32m$@\e[0;0m|\n"
endef
define print_executed_rule
	@printf "   +--->: |\e[0;32m$@\e[0;0m|${1}\n"
endef
define link_executable
	$(CC) -o $@ $^ ${1} ${2} ${3} ${4} $(LDFLAGS);
	$(call print_executed_rule,"=Executable\\n");
endef

$(LIB_NAME_OUT).a: help $(OBJECTS)
	$(call print_building_target);
	@ld -r $(OBJECTS) -o $(LIB_NAME_OUT).a

# Dynamic lib is not working yet. Need to compile with CFLAGS += -fPIC including assembly code
$(LIB_NAME_OUT).so: $(OBJECTS)
	$(call print_building_target);
	@ld -shared -o $(LIB_NAME_OUT).so $(OBJECTS)

$(LIB_NAME_OUT): $(LIB_NAME_OUT).a $(OBJECTS_UNITEST)
	$(call print_building_target);
	$(call link_executable)

$(OBJ_DIR)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(INCLUDES) $(DFLAGS) -o $@ -c $<
	$(call print_executed_rule)

$(OBJ_DIR)/%.o: %.S_shipped
	$(call print_building_target);
	@$(CC) -x assembler-with-cpp -o $@ -c $<

# ********** Clean *********
define clean_compilation_intermediate
	@printf "+-->Cleaning recursive dir |\e[1;45m${1}/*/.[od]\e[0;0m|\n"
	@find ./${1} -type f -name '*.[od]' -delete
endef
define clean_compilation_output
	@printf "+-->Cleaning |\e[1;45mLib / CrashDumps\e[0;0m|\n"
	@rm -f $(LIB_NAME_OUT) $(LIB_NAME_OUT).a $(LIB_NAME_OUT).so $(BACKUPS) core.*
endef

clean:
	@printf "+-->Cleaning |\e[1;45m$(LIB_NAME_OUT)\e[0;0m|\n"
	$(call clean_compilation_intermediate,$(OBJ_DIR_ROOT))
	$(call clean_compilation_output)

.PHONY: depend clean
-include $(DEPEND_FILES_ALL)
