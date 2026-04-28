# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# ********** Extract git information *********
ifeq ($(COMPILATION_DATE),)
	COMPILATION_DATE := NVMEIB_DATE\($(shell date +%-d,%-m,%Y)\)
endif
ifeq ($(HAS_LOCAL_GIT),)
	HAS_LOCAL_GIT=$(shell git rev-parse --is-inside-work-tree 2>/dev/null)
endif
ifeq ($(HAS_LOCAL_GIT),true)
	COMMIT_ID=$(shell git log -n1 --format=%h)
	CHANGE_ID=$(git log 2>/dev/null -n1 --format=%b | awk '/^Change-Id: / {print $2}')
	VER_TAGID=$(shell git describe --abbrev=0)
	BRANCH_NAME=$(shell git symbolic-ref 2>/dev/null --short --quiet HEAD || git rev-parse 2>/dev/null --abbrev-ref HEAD)
	NVMESH_RELEASE=$(shell git describe | cut -c 2- | head -c9)
endif
# Dummy commit info
ifeq ($(COMMIT_ID),)
	COMMIT_ID := 010101010
	CHANGE_ID := Unknown
	VER_TAGID := v0.0.0
	BRANCH_NAME := Unknown
	NVMESH_RELEASE :=0.0.0-000
endif

# ********** Define compiler and compilation flags *********
USE_CPP =0
USE_CLANG =0
UNAME := $(shell uname)# Which operating system we use?
HT = $(shell uname -p)
CFLAGS=
ifeq ($(UNAME),Darwin)
	USE_DARWIN=1
	CFLAGS += -m64
else ifeq ($(HT),x86_64)
	USE_64_BITS=1
	CFLAGS += -m64
else ifeq ($(HT),aarch64)
	USE_ARM=1
	CFLAGS += -march=armv8.1-a+crc+fp+simd
endif

GCC_VERSION := $(shell gcc -dumpversion)

USE_RELEASE?=0#Compile release
LDFLAGS = -rdynamic -no-pie
CFLAGS += $(EXTRA_CFLAGS)
CFLAGS += -Wno-address-of-packed-member
CFLAGS += -Wall -Werror -std=gnu99 -Wdeclaration-after-statement -Wstrict-prototypes#Simulate kernel compilation & enable all warning. Can use gnu89 instead of 99
CFLAGS += -Wextra -Wshadow -Werror=strict-aliasing -Wno-type-limits# Location of inline in function declaration

ADDRES_OF_PACKED_MIN_GCC_VERSION_CHECK := $(shell expr `printf '%s\n' "$(GCC_VERSION)" "9" | sort -V | head -n1` == "9")
ifeq "$(ADDRES_OF_PACKED_MIN_GCC_VERSION_CHECK)" "1"
	CFLAGS += -Wno-address-of-packed-member
endif

CFLAGS += -DNVMEIB_COMPILATION_DATE=${COMPILATION_DATE}
CFLAGS += -DCOMMIT_ID=0x$(COMMIT_ID)ULL -DCOMMIT_ID_STR=\"$(COMMIT_ID)\" -DVER_TAGID=$(VER_TAGID) -DBRANCH_NAME=$(BRANCH_NAME)
CFLAGS += -DNVMESH_VERSION=$(VER_TAGID) -DNVMESH_RELEASE=1.1.0-97 -DUSE_RELEASE=$(USE_RELEASE) -DBUILD_NUMBER=0.0
#CFLAGS += -Wformat=0 -fpermissive -Wno-conversion-null
#Make sure all functions are aligned to 8 bytes
CFLAGS += -falign-functions=8

ifeq ($(USE_CPP),1)
	CC=g++ #Compile as C++
	CFLAGS = -std=c++11 -fpermissive -w -Wextra -Wshadow -Werror=strict-aliasing
else #Compile as C
	ifeq ($(USE_CLANG),1)
		CFLAGS += -Wno-address-of-packed-member -Wno-unknown-attributes -Wno-unused-function -Wno-missing-braces
		CC=clang
	else
		CC=gcc
	endif
endif

ifeq ($(USE_SANITIZERS),1)
	CFLAGS += -fsanitize=leak -fsanitize=address -fsanitize=undefined
	LDFLAGS += -fsanitize=leak -fsanitize=address -fsanitize=undefined
endif

ifeq ($(USE_THREAD_SANITIZER),1)
	CFLAGS += -fsanitize=thread
	LDFLAGS += -fsanitize=thread
endif

ifeq ($(USE_COVERAGE),1)
	CFLAGS += --coverage -fprofile-abs-path -fprofile-update=atomic
	LDFLAGS += --coverage
endif

ifeq ($(USE_RELEASE),1)
	CFLAGS += -O2 -g3 -ggdb3 -Wno-array-bounds -fno-omit-frame-pointer -rdynamic
else
	CFLAGS += -g3 -ggdb3
endif

CFLAGS += -fvisibility=default
CFLAGS += -DBLKDEV_SIMULATOR=1 -DDEBUG_TOPO_CNTRS -DAUTONOMOUS_SYNCS_STATS -DDEBUG_TOMA_REG_LEAKS -DDEBUG_LOCKS_CORRUPTION
CFLAGS += -DNVMESH_IS_PRODUCTION_COMPILATION=0
CFLAGS += -DNVMEIBC_ENABLE_PER_VOLUME_STATS=1
#CFLAGS += -DDBGDI_REMOVED_IN_PRODUCTION
#CFLAGS += -DDEBUG_PERCPU_ISSUED_IO_CNTRS
#CFLAGS += -DDEBUG_TRANSFERS -DDEBUG_SUM
CFLAGS += -DNVMEIBC_SECTOR_SHIFT=12
CFLAGS += -DTRACE_CPUID
#CFLAGS += -DDEBUG_TOPO_DELAY_FREE_VERY_DANGEROUS -DDEBUG_LOCK_RETRY
CFLAGS += -DDEBUG_LOSER_CONDITIONS
#CFLAGS += -DCONFIG_NVMEIB_FINE
CFLAGS += -DTAKE_STATS
CFLAGS += -DBLKDEV_PROFILING
#CFLAGS += -DDEBUG_UNCOMPLETED
CFLAGS += -Winvalid-pch
#CFLAGS += -DDEBUG_SPINLOCKS
CFLAGS += -DTRACE_INCLUDE_FILE=\"nvmeibc_trace.h\"

# ********** Included directories of H files *********
# Main SSDA project directory. (relative to this Makefile)

THIS_MKFILE_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
NVMESH_ROOT_DIR_RLTV := $(THIS_MKFILE_DIR)/../../..
NVMESH_ROOT_DIR := $(realpath $(NVMESH_ROOT_DIR_RLTV))

SSDA = $(NVMESH_ROOT_DIR)
INCLUDES :=
INCLUDES += -I$(SSDA)/clnt/block/unitest -I$(SSDA)/clnt/ -I$(SSDA)/srv/ -I$(SSDA)/common/ -I$(SSDA)/common_public/ -I$(SSDA)/toma/ -I$(SSDA)/
INCLUDES += -I/usr/include
INCLUDES += -I$(SSDA)/autogen/toma/ -I$(SSDA)/autogen/common/ -I$(SSDA)/autogen/clnt/
INCLUDES += -I$(SSDA)/utils/nvmeib_jdr/

# ********** Define linker and compilation flags *********

ifeq ($(USE_ARM),1)		# Define directories of lib files
	LIBS_PATH = -L/usr/lib64
else ifeq ($(USE_DARWIN), 1)
	LIBS_PATH = -L/usr/lib
else ifeq ($(USE_64_BIT), 1)
	LIBS_PATH = -L/usr/lib64
endif

LIBS  = -ldl -lpthread -luuid # Libraries to link into executable (with -l): pthread is needed

# ********** Define Files participating in the build *********
# Simulator files
SOURCES  = tests_conf.c
SOURCES += bunitest.c kr_incs.c nvmeibc_simu_disk.c ib_incs.c kr_sim_svc.c nvmeib_common_all.c nvmesh_sim.c di_tracker.c
SOURCES += memmgr_metrics_tests.c
SOURCES += nvmeibc_volume_targets_tests.c
SOURCES += metrics_test.c
SOURCES += wq_metrics_tests.c
SOURCES += safe_casting_tests.c
SOURCES += error_tags_tests.c
SOURCES += kr_incs_bit_ops_test.c
SOURCES += io_pet_traces_controller.c
SOURCES += ./clnt/nvmeibc_cli_scripts_simu.c ./clnt/nvmeibc_user_space_simu.c
SOURCES += ./clnt/nvmeibc_nvmeiba_kapi_sim.c
SOURCES += ./mgmt/nvmeibm_conf_db.c ./mgmt/nvmeibm_mgmt_simu.c ./mgmt/nvmeibm_mcs_simu.c
SOURCES += ./toma/nvmeibt_topology_simu.c ./toma/nvmeibt_toma_simu.c ./toma/nvmeibr_disk_metadata_sim.c
SOURCES += ./server/nvmeibs_ssd_disk_sim.c ./server/nvmeibs_nic_dma_atomics.c ./server/nvmeibs_main_sim.c
SOURCES += ./server/nvmeibs_serjio_deps_sim.c
SOURCES += ./uni_framework/bunitest_conf.c
SOURCES += ./uni_framework/kr_incs_unitest.c
SOURCES += ./uni_scenarios/uni_scenario_ec.c
SOURCES += ./uni_scenarios/uni_scenario_vol_config.c
SOURCES += ./uni_scenarios/uni_scenario_tx_history_ec_jdr.c
SOURCES += ./uni_scenarios/uni_scenario_tx_history_ec.c
SOURCES += ./uni_scenarios/uni_scenario_gf.c
SOURCES += ./uni_scenarios/uni_enumerators.c
SOURCES += ./uni_scenarios/uni_random.c
SOURCES += ./uni_scenarios/uni_recoveries.c
SOURCES += ./uni_scenarios/uni_scenario_io_perm_alert.c
SOURCES += ./uni_scenarios/uni_scenario_ec_jam.c
SOURCES += ./uni_scenarios/uni_scenario_ec_serjio.c
SOURCES += ./uni_scenarios/uni_scenario_common.c
SOURCES += ./nvmeibc_tracer_unitest.c
SOURCES += ./nvmeib_scatterlist_iter_test.c
SOURCES += ./nvmeibc_management_capi_parse_conf_test.c

# Production code: /utils/
UTILS_DIR := $(SSDA)/utils
SOURCES += $(UTILS_DIR)/nvmeib_jdr/nvmeib_jdr.c
SOURCES += $(UTILS_DIR)/nvmeib_jdr/nvmeib_txt.c

# Production code: /common/
CMDIR = $(SSDA)/common
SOURCES += $(CMDIR)/nvmeib_io_stats.c $(CMDIR)/nvmeib_mcs_msg_cache.c $(CMDIR)/nvmeib_mcs.c $(CMDIR)/nvmeib_kth_events.c
SOURCES += $(CMDIR)/nvmeib_json.c
SOURCES += $(CMDIR)/nvmeib_metrics_jdr.c
SOURCES += $(CMDIR)/nvmeib_memmgr_metrics.c
SOURCES += $(CMDIR)/nvmeib_wq_metrics.c
SOURCES += $(CMDIR)/nvmeib_error_tags.c
SOURCES += $(CMDIR)/nvmeib_pcpu_wq.c
SOURCES += $(CMDIR)/proc_epilog.c
SOURCES += $(CMDIR)/nvmeib_scatterlist_iter.c
SOURCES += $(SSDA)/common_public/nvmeib_public_procfs.c
SOURCES += $(SSDA)/common_public/nvmeib_jdr_proc.c
SOURCES += $(SSDA)/common_public/kth/nvmeib_public_kth.c
SOURCES += $(SSDA)/common_public/nvmeib_event.c

# Production code: /clnt/block/
CDIR = $(SSDA)/clnt/block
SOURCES += $(CDIR)/nvmeibc_block_api_os.c $(CDIR)/nvmeibc_topology.c
SOURCES += $(CDIR)/nvmeibc_block_api_conf.c $(CDIR)/nvmeibc_block_api_nvmeshioctl.c
SOURCES += $(CDIR)/controlpath/nvmeibc_b_cp_lost_srv_resources.c
SOURCES += $(CDIR)/controlpath/nvmeibc_b_cp_cpu_masks.c
SOURCES += $(CDIR)/controlpath/nvmeibc_b_cp_send_toma_msg.c
SOURCES += $(CDIR)/controlpath/nvmeibc_b_cp_topo_on_free.c
SOURCES += $(CDIR)/controlpath/nvmeibc_b_cp_topo_seg_on_active.c
SOURCES += $(CDIR)/controlpath/nvmeibc_b_cp_trs_hash.c
SOURCES += $(CDIR)/datapath_ec/nvmeibc_block_dp_ec.c
SOURCES += $(CDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_arm_neon4.c
SOURCES += $(CDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_arm_recov_neon.c
SOURCES += $(CDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_arm_um.c
SOURCES += $(CDIR)/datapath_ec/nvmeibc_block_dp_ec_gf.c
SOURCES += $(CDIR)/datapath_ec/nvmeibc_block_dp_ec_journal.c
SOURCES += $(CDIR)/datapath_ec/nvmeibc_block_dp_ec_reed_solomon.c
SOURCES += $(CDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_praid.c
SOURCES += $(CDIR)/datapath_ec/recov/nvmeibc_block_dp_ec_recov_cold.c
SOURCES += $(CDIR)/datapath_ec/recov/nvmeibc_block_dp_ec_recov_cold_sync.c
SOURCES += $(CDIR)/datapath_ec/recov/nvmeibc_block_dp_ec_recov_hot.c
SOURCES += $(CDIR)/datapath_ec/recov/nvmeibc_block_dp_ec_sync_txid_wraparound.c
SOURCES += $(CDIR)/datapath_ec/recov/nvmeibc_block_dp_ec_recov_maintenance.c
SOURCES += $(CDIR)/datapath_ec/recov/nvmeibc_block_dp_ec_recovery_common.c
SOURCES += $(CDIR)/datapath_mirror/nvmeibc_block_dp_mirror.c
SOURCES += $(CDIR)/datapath_mirror/nvmeibc_block_dp_mirror_trim.c
SOURCES += $(CDIR)/datapath_mirror/nvmeibc_block_dp_mirror_sync.c
SOURCES += $(CDIR)/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_buffers.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_cmd_lock_link.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_io_resubmitter.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.c
SOURCES += $(CDIR)/datapath_utils_generic/operation/nvmeibc_block_dp_operation_locks_transfer.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_io_perm_alert.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_common.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_cpu_masks.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_operation.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_lock_server.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_dbits.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_locks_scheme.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_block_md_tables.c
SOURCES += $(CDIR)/datapath_utils_generic/nvmeibc_block_dp_block_md.c
SOURCES += $(CDIR)/datapath_utils_generic/profiling/nvmeibc_block_dp_profiling_generic.c
SOURCES += $(CDIR)/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.c
SOURCES += $(CDIR)/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_log.c
SOURCES += $(CDIR)/recovery/nvmeibc_raid_recovery.c
SOURCES += $(CDIR)/recovery/nvmeibc_raid_recov_itr.c
SOURCES += $(CDIR)/recovery/nvmeibc_block_dp_sync_common.c
SOURCES += $(CDIR)/recovery/nvmeibc_decentralized_unreg.c
SOURCES += $(CDIR)/recovery/nvmeibc_block_dp_sync_no_write_hole.c
SOURCES += $(CDIR)/recovery/nvmeibc_block_dp_sync_api_manager_flow_cntrs.c

# Production code: utilities
SOURCES += $(SSDA)/perfTest/io_stress/cmp_blocks/cmp_blocks_impl.c

# Production code: /srv/
SOURCES += $(SSDA)/srv/nvmeibs_main_gen_cmds.c
SOURCES += $(SSDA)/srv/nvmeibs_nordda.c
SOURCES += $(SSDA)/srv/nvmeibs_serjio.c
SOURCES += $(SSDA)/srv/nvmeibs_serjio_stats.c
SOURCES += $(SSDA)/srv/nvmeibs_serjio_gen_cmd_handlers.c
# Production code: /atom/
ADIR = $(SSDA)/clnt/atom
SOURCES += $(ADIR)/nvmeiba_infra.c $(ADIR)/nvmeiba_atom_os_api.c $(ADIR)/nvmeiba_main.c

# Production code: /clnt/
SDIR = $(SSDA)/clnt
SOURCES += $(SDIR)/nvmeibc_error_tags.c
SOURCES += $(SDIR)/nvmeibc_block.c
SOURCES += $(SDIR)/nvmeibc_pausable.c
SOURCES += $(SDIR)/nvmeibc_client_core_ops.c
SOURCES += $(SDIR)/nvmeibc_jam.c
SOURCES += $(SDIR)/nvmeibc_memmgr_metrics.c
SOURCES += $(SDIR)/nvmeibc_wq_metrics.c
SOURCES += $(SSDA)/toma/utils/nvmeibt_uuid.c
SOURCES += $(SSDA)/toma/clnt/nvmeibt_client_protocol.c
SOURCES += $(SDIR)/module/instance/nvmeibc_cinst.c

SOURCES += $(SDIR)/nvmeibc_volume.c $(SDIR)/nvmeibc_main.c $(SDIR)/nvmeibc_common.c $(SDIR)/nvmeibc_cc_api.c $(SDIR)/nvmeibc_io_pet.c $(SDIR)/nvmeibc_targets.c
SOURCES += $(SDIR)/nvmeibc_volume_targets.c
SOURCES += $(SDIR)/nvmeibc_disk_gen_cmds.c
SOURCES += $(SDIR)/nvmeibc_capabilities.c
SOURCES += $(SDIR)/management_utils_common/nvmeibc_management_capi_parse_conf.c
SOURCES += $(SDIR)/management_utils_common/nvmeibc_management_volume_conf_checks.c
#SOURCES += $(SDIR)/nvmeibc_ib_nordda_channel.c

# Enable internal binary tracing by default
SOURCES += $(SSDA)/tools/nvmeib_trace_userspace.c $(SSDA)/tools/nvmeib_trace_userspace_poller.c
CFLAGS += -I$(SSDA)/tools/ -DUSER_SPACE_TRACING

ifeq ($(USE_ARM), 1)
	#ASM_SRC	= $(CDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_asm_arm.S_shipped
	ARM_SRC :=
else
	SOURCES += $(CDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_avx2.c
	CFLAGS_nvmeibc_block_dp_ec_gf_avx2.c += -mavx2 -msse4.2
	ASM_SRC :=
endif
ASM_OBJ = $(ASM_SRC:.S_shipped=.o)

OBJ_DIR_ROOT=99bin
ifeq ($(BUILD_RELEASE),1)
	OBJ_DIR=./$(OBJ_DIR_ROOT)/release
else
	OBJ_DIR=./$(OBJ_DIR_ROOT)/debug
endif

OBJECTS = $(patsubst %.c, $(OBJ_DIR)/%.o, $(SOURCES)) $(addprefix $(OBJ_DIR)/,$(ASM_OBJ))
COMPILATION_CMD_FILE = $(OBJ_DIR)/compilation_cmd.txt
BACKUPS =$(SOURCES:.c=.~) $(ASM_OBJ:.o=.~)
EXE_NAME = blk_unitest
EXE_PATH = $(THIS_MKFILE_DIR)/$(EXE_NAME)

PRECOMPILED_HEADERS := $(OBJ_DIR)/pch.h.gch
IGNORE_PRECOMPILED_HEADERS := kr_incs.c tests_conf.c $(SSDA)/tools/nvmeib_trace_userspace.c $(SSDA)/tools/nvmeib_trace_userspace_poller.c nvmeibc_block_dp_ec_gf_avx2.c
FILES_DEPEND =$(OBJECTS:.o=.d) $(PRECOMPILED_HEADERS:.h.gch=.h.d)

# Autogen
AUTOGEN_DIR=$(abspath $(SSDA)/autogen)
CLNT_AUTOGEN_DIR=$(AUTOGEN_DIR)/clnt/
TOMA_AUTOGEN_DIR=$(AUTOGEN_DIR)/toma/
COMN_AUTOGEN_DIR=$(AUTOGEN_DIR)/common/
PROC_SCHEME_FILE=../management_cm/clnt/process_scheme.py
CLNT_SCHEME_FILE=../management_cm/clnt/clnt_scheme.json
TOMA_SCHEME_FILE=../management_cm/toma_scheme.json
PROC_SCHEME_PATH=$(AUTOGEN_DIR)/$(PROC_SCHEME_FILE)
CLNT_SCHEME_PATH=$(AUTOGEN_DIR)/$(CLNT_SCHEME_FILE)
TOMA_SCHEME_PATH=$(AUTOGEN_DIR)/$(TOMA_SCHEME_FILE)

ifneq ($(MAKECMDGOALS),clean)
  -include $(FILES_DEPEND)
endif

# ********** Define Rules for building *********
.PHONY: depend clean gf_perf post_lttng write_compilation_cmd autogen ECHO_COMPILING pet_dictionary

$(OBJ_DIR)/%.o : %.S_shipped
	gcc -x assembler-with-cpp -c $< -o $@

all: $(SOURCES) $(EXE_NAME) pet_dictionary

AUTOGEN_TARGETS = $(AUTOGEN_DIR)/toma/nvmeibt_mcs_stub.h $(AUTOGEN_DIR)/clnt/nvmeibc_mcs_stub.h $(AUTOGEN_DIR)/common/nvmeib_mcs_header.h

$(AUTOGEN_DIR)/common/nvmeib_mcs_header.h: $(PROC_SCHEME_PATH)
	@echo $(AUTOGEN_DIR)/common/nvmeib_mcs_header.h
	@cd $(AUTOGEN_DIR) && python3 $(PROC_SCHEME_FILE) --header ./common/nvmeib_mcs_header.h

$(AUTOGEN_DIR)/clnt/nvmeibc_mcs_stub.h: $(PROC_SCHEME_PATH) $(CLNT_SCHEME_PATH)
	@echo $(AUTOGEN_DIR)/clnt/nvmeibc_mcs_stub.h
	@cd $(AUTOGEN_DIR) && python3 $(PROC_SCHEME_FILE) $(CLNT_SCHEME_FILE) ./clnt/nvmeibc_mcs_stub.h

$(AUTOGEN_DIR)/toma/nvmeibt_mcs_stub.h: $(PROC_SCHEME_PATH) $(TOMA_SCHEME_PATH)
	@echo $(AUTOGEN_DIR)/toma/nvmeibt_mcs_stub.h
	@cd $(AUTOGEN_DIR) && python3 $(PROC_SCHEME_FILE) $(TOMA_SCHEME_FILE) ./toma/nvmeibt_mcs_stub.h

autogen: $(AUTOGEN_TARGETS) | ECHO_COMPILING
	@printf "Autogen code \e[0;32m Done\e[0;0m\n"


PET_DICT_DIR = $(THIS_MKFILE_DIR)/$(OBJ_DIR)/io_pet
PET_DICTIONARY = $(PET_DICT_DIR)/dict.$(COMMIT_ID).json
PET_MESSAGES_SH = $(SSDA)/common/pet/pet_messages.sh

$(EXE_NAME): $(OBJECTS) $(ASMOBJ) $(OBJ_DIR)/traces_ids.o
	@$(CC) $(LDFLAGS) $(OBJECTS) $(OBJ_DIR)/traces_ids.o -o $(EXE_NAME) $(LIBS_PATH) $(LIBS)
	$(info -->Linking  ... |$(OBJECTS)|)

pet_dictionary: $(EXE_NAME)
	@mkdir -p $(PET_DICT_DIR)
	-$(PET_MESSAGES_SH) save-dictionary $(EXE_PATH) nvmeibc_io_pet_msgs $(PET_DICTIONARY)

$(OBJECTS): $(PRECOMPILED_HEADERS)

$(PRECOMPILED_HEADERS): $(OBJ_DIR)/gen_events.h

$(OBJ_DIR)/%.h.gch: %.h $(COMPILATION_CMD_FILE) | autogen
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -MD -MP -o $@
	@printf "Making precompiled header \e[0;32m $@\e[0;0m, Done\n"

# ********** Bin traces / lib /utils *********
INCLUDES += -I$(SSDA)/clnt/block/unitest/$(OBJ_DIR) 		# To access gen_events.h file
# The lines below are used to generate unique macro __FILE_LITERAL__ for each source file that evaluates to file_name_c without "
define set_per_file_cflags =
	CFLAGS_$1 += -D__FILE_LITERAL__=$(1:.c=_c)
endef
$(foreach prog,$(SOURCES),$(eval $(call set_per_file_cflags,$(shell basename $(prog)))))
define per_file_cflags_varval =
	$(CFLAGS_$(shell basename $1))
endef

$(OBJ_DIR)/%.i: %.c $(COMPILATION_CMD_FILE) | autogen
	@echo $@
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(call per_file_cflags_varval,$<) $(INCLUDES) -E -D__FIRST_PASS__ $< | grep -v '#define' > $@

FMTLIB := libfmtrs.so
PREPROCESSED =$(patsubst %.c, $(OBJ_DIR)/%.i, $(SOURCES))
TRACER_PP := $(abspath $(SSDA)/tools/pre_processor/tracer_pp.py)
TRACE_GEN_SCRIPT = $(SSDA)/tools/pre_processor/gen_probes2.py
TRACE_DICTIONARY = $(SSDA)/tools/dictionary.json
MERGE_DICTIONARY := $(abspath $(SSDA)/tools/merge_dictionary.sh)
SIM_DICT_CKSUM := 5 #  Set a number to be able to compile with a different dictionary
GEN_EV_MD5=$(OBJ_DIR)/gen_events.h.md5
FILE_TR_JSON=$(OBJ_DIR)/nvmeibc.trace.json

$(OBJ_DIR)/$(FMTLIB):
	make SSDA=$(SSDA) -C $(SSDA)/tools/traces_post_processor/formatters
	cp $(SSDA)/tools/traces_post_processor/formatters/$(FMTLIB) $(OBJ_DIR)/$(FMTLIB)

$(OBJ_DIR)/gen_events.h: $(COMPILATION_CMD_FILE) $(FILE_TR_JSON) $(OBJ_DIR)/$(FMTLIB) $(TRACE_GEN_SCRIPT)
	@printf "Making \e[0;32m $(OBJ_DIR)/gen_events.h\e[0;0m\n"
	@# compile the extern id traces file
	@$(TRACE_GEN_SCRIPT) $(FILE_TR_JSON) nvmeibc -o $@ --userspace --no_base_includes
	@# Calculate md5, update timestamp only if content changed
	@md5sum $(OBJ_DIR)/gen_events.h | cmp -s $(GEN_EV_MD5) -; if test $$? -ne 0; then md5sum $(OBJ_DIR)/gen_events.h > $(GEN_EV_MD5); fi
	@# Use same timestamp for autogenerated .h files as .md5 file
	@touch -r $(GEN_EV_MD5) $(OBJ_DIR)/gen_events.h
	@printf "Done \e[0;32m $(OBJ_DIR)/gen_events.h\e[0;0m\n"

$(FILE_TR_JSON): $(TRACER_PP) $(MERGE_DICTIONARY) $(TRACE_GEN_SCRIPT) $(TRACE_DICTIONARY) $(PREPROCESSED)
	@echo make $(FILE_TR_JSON)
	@$(TRACER_PP) $(FILE_TR_JSON) $(PREPROCESSED)
	@$(MERGE_DICTIONARY) $(FILE_TR_JSON) $(TRACE_DICTIONARY) --cksum $(SIM_DICT_CKSUM)

$(OBJ_DIR)/%.o: %.c $(COMPILATION_CMD_FILE) | autogen
	@if echo "$(IGNORE_PRECOMPILED_HEADERS)" | tr ' ' '\n' | grep -q $(notdir $<) ; \
	then echo $@; $(CC) $(CFLAGS) $(call per_file_cflags_varval,$<) $(INCLUDES) -c $< -MD -MP -o $@; \
	else echo $@; $(CC) $(CFLAGS) $(call per_file_cflags_varval,$<) $(INCLUDES) -c $< -include $(PRECOMPILED_HEADERS:.h.gch=.h) -MD -MP -o $@; \
	fi

$(OBJ_DIR)/traces_ids.c: $(OBJ_DIR)/gen_events.h

$(OBJ_DIR)/traces_ids.o : $(OBJ_DIR)/traces_ids.c
	@printf "Making \e[0;32m traces_ids\e[0;0m\n"
	$(CC) $(CFLAGS) -c $< -o $@
	@printf "Making \e[0;32m traces_ids\e[0;0m, done\n"

# ********** Bin traces done *********

ECHO_COMPILING:
	$(info -->UNAME=$(UNAME), CPU=$(HT), USE_ARM=$(USE_ARM))
	$(info -->Compilation info)
	$(info -->CFLAGS | $(CFLAGS))
	$(info -->LDLAGS | $(LDFLAGS))
	$(info -->Compiler Version | $(shell $(CC) --version))
#	$(info OBJECTS --> $(OBJECTS))
	$(info ===========================================)
	$(info )

$(ASMOBJ) : $(ASMSRC)
	@$(MAKE) -C $(CDIR)/datapath_ec $@

PERF_SRC = $(CDIR)/datapath_ec/nvmeibc_block_dp_ec_gf.c $(CDIR)/datapath_ec/nvmeibc_block_dp_ec_gf_avx2.c ./uni_scenarios/uni_scenario_gf.c ./kr_incs.c ./kr_sim_svc.c
PERF_OBJ = $(PERF_SRC:.c=.o)
ifneq ($(strip $(ASM_OBJ)),)
PERF_OBJ += $(OBJ_DIR)/$(ASM_OBJ)
endif
PERF_NAME= gf_perf

gf_perf:
	@$(foreach obj, $(PERF_OBJ), $(CC) $(CFLAGS) -O4 -DGF_PERF $(INCLUDES) -c $(obj:.o=.c)  -o $(obj) ; )
	@$(MAKE) -C $(CDIR)/datapath_ec
	@$(CC) $(LDFLAGS) $(PERF_OBJ) -o $(PERF_NAME) $(LIBS_PATH) $(LIBS)
	@$(info -->Linking  ... |$(PERF_OBJ)|)

clean_prev_run:
	@printf "+-->Cleaning |\e[1;45mprev run output\e[0;0m|\n"
	@rm -f core.* long*.txt *.binlog0.* out.txt
# To start over from scratch, type 'make clean'. This removes the executable file, old object files, backup files and logs of the previous execution
clean: clean_prev_run
	@printf "+-->Cleaning |\e[1;45mcompilation results\e[0;0m|\n"
	@rm -f $(OBJECTS) $(BACKUPS) $(EXE_NAME)
	@rm -f $(EXE_NAME).log $(EXE_NAME).err
	@rm -f $(PERF_OBJ) $(PERF_NAME)
	@rm -f $(PREPROCESSED) $(OBJ_DIR)/gen_events.h $(FILE_TR_JSON) $(PET_DICTIONARY)
	@rm -f $(PRECOMPILED_HEADERS)
	@rm -f $(COMPILATION_CMD_FILE) $(FILES_DEPEND) $(AUTOGEN_TARGETS)
	@printf "+-->Removing |\e[1;45m$(OBJ_DIR)\e[0;0m|\n"
	@rm -rf $(OBJ_DIR)

print_src_list:
	@echo $(SOURCES)

write_compilation_cmd:
	@mkdir -p $(OBJ_DIR)
	@printf "Writing \e[0;32m$(OBJ_DIR)/compilation_cmd.txt\e[0;0m\n"
	@echo $(CC) $(CFLAGS) $(INCLUDES) | cmp -s $(COMPILATION_CMD_FILE) -; if test $$? -ne 0; then echo $(CC) $(CFLAGS) $(INCLUDES) > $(COMPILATION_CMD_FILE); fi

$(OBJ_DIR)/compilation_cmd.txt : write_compilation_cmd
