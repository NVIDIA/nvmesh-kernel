# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Make files tutorial: http://www.cs.swarthmore.edu/~newhall/unixhelp/howto_makefiles.html
# ********** Define compiler and compilation flags *********
ARCH := $(shell uname -m)
BUILD_RELEASE?=0
UNAME := $(shell uname)# Which operating system we use?
CFLAGS  = -Wall -Werror -std=gnu99 -Wdeclaration-after-statement -Wstrict-prototypes -Wdeclaration-after-statement
CFLAGS += -Wextra -Wshadow -Werror=strict-aliasing -Wno-address-of-packed-member -Wno-type-limits# Location of inline in function declaration
CFLAGS += -DNVMEIB_COMPILATION_DATE=${COMPILATION_DATE}
CFLAGS += -Wno-missing-braces -Wno-missing-field-initializers
#CFLAGS += -Wformat=0 -fpermissive -Wno-conversion-null -Wno-permissive
ifeq ($(USE_CLANG)_$(shell clang -v 2>&1 | grep -c "clang version"), 1_1)
	CC=clang
else
	CC=gcc
	CFLAGS += -Wno-old-style-declaration
endif

ifeq ($(BUILD_RELEASE),1)
	CFLAGS += -O3 -g -DNDEBUG
else
	CFLAGS += -g3 -ggdb3
endif

CFLAGS_x86_64 = -m64  # Always compile as 64 bit in x86
CFLAGS_aarch64 = -march=armv8-a+crc # for crc32cx used in kr_incs_crc32.inc.c
CFLAGS += $(CFLAGS_$(ARCH))

# ********** Extract git information *********
ifeq ($(HAS_LOCAL_GIT),)
    HAS_LOCAL_GIT=$(shell git rev-parse --is-inside-work-tree)
endif
ifeq ($(HAS_LOCAL_GIT),true)
    COMMIT_ID=$(shell git log -n1 --format=%h)
    VER_TAGID=$(shell git describe --abbrev=0)
    BRANCH_NAME=$(shell git symbolic-ref 2>/dev/null --short --quiet HEAD || git rev-parse 2>/dev/null --abbrev-ref HEAD)
    #CHANGE_ID=$(git log 2>/dev/null -n1 --format=%b | awk '/^Change-Id: / {print $2}')
endif
# Dummy commit info
ifeq ($(COMMIT_ID),)
    COMMIT_ID := 010101010
    VER_TAGID := v3.1.0
    BRANCH_NAME := Unknown
endif

CFLAGS += -fvisibility=default
#CFLAGS += -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-unused-function #Bin trances disabled
CFLAGS += -DTOMA_SIMULATOR_SANDBOX -DUSER_SPACE
CFLAGS += -D__USE_GNU -D_GNU_SOURCE
CFLAGS += -DNVMEIBC_SECTOR_SHIFT=12
CFLAGS += -DGIT_COMMIT_ID=\"0x$(COMMIT_ID)\" -DVER_TAGID=$(VER_TAGID) -DGIT_BRANCH=\"$(BRANCH_NAME)\"
CFLAGS += -DTOMA -DNVMESH_VERSION=$(VER_TAGID) -DNVMESH_RELEASE=2.0.0-1982 -DBUILD_VERSION_FOR_MGMT=\"mmmmmmm1\" -DBUILD_NUMBER_FOR_MGMT=\"mmmmmmm2\"
# When git commit changes, invalidate pch
CFLAGS += -I$(SSDA)/tools/

# ********** Included directories of H files *********
# Main SSDA project directory. (relative to this Makefile)
SSDA=$(realpath ../..)
INCLUDES= -I/usr/include -I$(SSDA)/ -I$(SSDA)/toma/ -I$(SSDA)/autogen/common/ -I$(SSDA)/autogen/toma/

# ********** Define linker and compilation flags *********
LDFLAGS = -rdynamic
ifeq ($(UNAME), Darwin)
	LIBS_PATH = -L/usr/lib
else
	LIBS_PATH = -L/usr/lib64
endif
LIBS  = -lpthread -lm -ldl

USE_SANITIZERS?=1
ifeq ($(USE_SANITIZERS),1)
	CFLAGS += -fsanitize=leak -fsanitize=address -fsanitize=undefined
	LDFLAGS += -fsanitize=leak -fsanitize=address -fsanitize=undefined
# Suppress address sanitizer alignment warnings in gpt_util.c
	CFLAGS += -fno-sanitize=alignment
	LDFLAGS += -fno-sanitize=alignment
endif

# ********** Define Files participating in the build *********
# Simulator files
SOURCES :=
SOURCES += ./toma_in_sandbox.c

TDIR=./..
#TDIR=$(realpath ./..)
CMDIR= ./../../common

# Production code: /toma/, Generated with 'ls -1a *.c'
SOURCES += \
$(TDIR)/nvmeibt_block_device.c \
$(TDIR)/nvmeibt_bm.c \
$(TDIR)/nvmeibt_chunk.c \
$(TDIR)/nvmeibt_common.c \
$(TDIR)/nvmeibt_debug.c \
$(TDIR)/nvmeibt_disk_metadata.c \
$(TDIR)/nvmeibt_disk_segment_basics.c \
$(TDIR)/nvmeibt_disk_segment.c \
$(TDIR)/nvmeibt_disk.c \
$(TDIR)/nvmeibt_ds_metadata.c \
$(TDIR)/nvmeibt_global.c \
$(TDIR)/nvmeibt_kafka.c \
$(TDIR)/nvmeibt_km_comm.c \
$(TDIR)/nvmeibt_local_disk_util.c \
$(TDIR)/nvmeibt_local_disk.c \
$(TDIR)/nvmeibt_local_nic.c \
$(TDIR)/nvmeibt_mm_json.c \
$(TDIR)/nvmeibt_nic.c \
$(TDIR)/nvmeibt_node.c \
$(TDIR)/nvmeibt_praid_basics.c \
$(TDIR)/nvmeibt_praid.c \
$(TDIR)/nvmeibt_raft.c \
$(TDIR)/nvmeibt_read_config.c \
$(TDIR)/nvmeibt_recovery.c \
$(TDIR)/nvmeibt_register.c \
$(TDIR)/nvmeibt_rpc.c \
$(TDIR)/nvmeibt_seg_active.c \
$(TDIR)/nvmeibt_str.c \
$(TDIR)/nvmeibt_toma.c \
$(TDIR)/nvmeibt_topo_bin.c \
$(TDIR)/nvmeibt_topology.c \
$(TDIR)/nvmeibt_utils.c \
$(TDIR)/nvmeibt_uuid.c \
$(TDIR)/nvmeibt_wq.c \
$(TDIR)/clnt/nvmeibt_client_protocol.c \
$(TDIR)/clnt/nvmeibt_client.c \
$(TDIR)/local/ram/nvmeibt_ds_blkset_entries.c \
$(TDIR)/utils/gpt_util.c \

SOURCES += \
$(TDIR)/interfaces/log/nvmeibt_binary_tracing.c \
$(TDIR)/../tools/nvmeib_trace_userspace.c \
$(TDIR)/../tools/nvmeib_trace_userspace_poller.c \
$(TDIR)/interfaces/srvr/nvmeibt_srvr_proc.c \

#$(TDIR)/interfaces/os/nvmeibt_os_signal.c

BACKUPS =$(SOURCES:.c=.~)
EXE_NAME = nvmeibt_toma

OBJ_DIR_ROOT=99bin
ifeq ($(BUILD_RELEASE),1)
    OBJ_DIR=./$(OBJ_DIR_ROOT)/release/obj
else
    OBJ_DIR=./$(OBJ_DIR_ROOT)/debug/obj
endif
$(shell mkdir -p $(OBJ_DIR))
OBJECTS =$(patsubst %.c, $(OBJ_DIR)/%.o, $(SOURCES))
DEPEND_FILES_ALL = $(OBJECTS:.o=.d)

# ********** Define Rules for building *********
define print_compilation_info
	@printf "===========================================\n"
	@printf "\e[0;32mCompilation info\e[0;0m: Release=$(BUILD_RELEASE), COMMIT_ID=$(COMMIT_ID), $(CC), TRACE_LEVEL=$(TRACE_LEVEL) HT=$(HT)|\n"
    @printf "\t* Proj    | $(ROOT_DIR_ABS) [$(ROOT_DIR_RLTV)]\n"
    @printf "\t* CFLAGS  | $(CFLAGS)\n"
    @printf "\t* INCLUDS | $(INCLUDES)\n"
    @printf "\t* DFLAGS  | $(LDFLAGS)\n"
    @printf "===========================================\n"
endef

help:
	$(call print_compilation_info);

all: $(SOURCES) autogen $(EXE_NAME)
	$(call print_compilation_info)
	$(info +--->100% Done!)

# ********** Bin traces / lib /utils *********
INCLUDES += -I$(OBJ_DIR)
SSDA = $(realpath $(TDIR)/..)
MERGE_MULTI_GEN := $(abspath $(SSDA)/tools/pre_processor/merge_multi_gen_events.py)
TRACER_PP := $(abspath $(SSDA)/tools/pre_processor/tracer_pp.py)
TRACE_GEN_SCRIPT = $(SSDA)/tools/pre_processor/gen_probes2.py
MERGE_DICTIONARY := $(abspath $(SSDA)/tools/merge_dictionary.sh)
FILE_TR_JSON=$(OBJ_DIR)/nvmeibt.trace.json
PWD=$(shell pwd)

define set_per_file_cflags =
	CFLAGS_$1 += -D__FILE_LITERAL__=$(1:.c=_c)
endef
$(foreach prog,$(SOURCES),$(eval $(call set_per_file_cflags,$(shell basename $(prog)))))
define per_file_cflags_varval =
	$(CFLAGS_$(shell basename $1))
endef

FMTLIB := libfmtrs.so
$(OBJ_DIR)/$(FMTLIB):
	make SSDA=$(SSDA) -C $(SSDA)/tools/traces_post_processor/formatters
	cp $(SSDA)/tools/traces_post_processor/formatters/$(FMTLIB) $(OBJ_DIR)/$(FMTLIB)

$(OBJ_DIR)/%.i: %.c
	@echo "Preprocessing $< -> $@"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(call per_file_cflags_varval,$<) $(INCLUDES) -E -D__FIRST_PASS__ -D"__attribute__(x)=" -D_NVMEIB_TRACE_BACKEND_USER $< -o $@

%.trace.json: %.i $(MERGE_DICTIONARY) $(TRACER_PP)
	@echo "Generating trace JSON $@"
	@mkdir -p $(dir $@)
	@$(TRACER_PP) -j 1 $@ $<
	@$(MERGE_DICTIONARY) $@ $(SSDA)/tools/dictionary.json

%.c_gen_events.h: %.trace.json $(TRACE_GEN_SCRIPT)
	@echo "Generating gen_events header $@"
	@mkdir -p $(dir $@)
	@cd $(dir $@); $(TRACE_GEN_SCRIPT) $(PWD)/$< nvmeibt --userspace -o $(PWD)/$@

# ********** Define Rules for Autogen *********
AUTOGEN_DIR=$(abspath $(SSDA)/autogen)
PROC_SCHEME_FILE=../management_cm/clnt/process_scheme.py
CLNT_SCHEME_FILE=../management_cm/clnt/clnt_scheme.json
TOMA_SCHEME_FILE=../management_cm/toma_scheme.json
PROC_SCHEME_PATH=$(AUTOGEN_DIR)/$(PROC_SCHEME_FILE)
AUTOGEN_TARGETS = $(AUTOGEN_DIR)/toma/nvmeibt_mcs_stub.h $(AUTOGEN_DIR)/clnt/nvmeibc_mcs_stub.h

$(AUTOGEN_DIR)/clnt/nvmeibc_mcs_stub.h: $(PROC_SCHEME_PATH) $(AUTOGEN_DIR)/$(CLNT_SCHEME_FILE)
	@echo $@
	@cd $(AUTOGEN_DIR) && python3 $(PROC_SCHEME_FILE) $(CLNT_SCHEME_FILE) ./clnt/nvmeibc_mcs_stub.h

$(AUTOGEN_DIR)/toma/nvmeibt_mcs_stub.h: $(PROC_SCHEME_PATH) $(AUTOGEN_DIR)/$(TOMA_SCHEME_FILE)
	@echo $@
	@cd $(AUTOGEN_DIR) && python3 $(PROC_SCHEME_FILE) $(TOMA_SCHEME_FILE) ./toma/nvmeibt_mcs_stub.h

autogen: $(AUTOGEN_TARGETS)
	@printf "Autogen code \e[0;32m Done\e[0;0m\n"

# ********** Autogen done *********
$(EXE_NAME): $(OBJECTS) $(OBJ_DIR)/$(FMTLIB)
# Merge all individual trace JSONs
	@printf "Merging all trace JSONs to \e[0;32m $(FILE_TR_JSON)\e[0;0m\n"
	@$(MERGE_MULTI_GEN) $(FILE_TR_JSON) $(OBJ_DIR_ROOT)/$(BDIR) $(OBJECTS)
# Generate final gen_events.h for traces_ids.c (with only trace IDs)
	@printf "Making \e[0;32m gen_events.h\e[0;0m for trace IDs\n"
	@$(TRACE_GEN_SCRIPT) $(FILE_TR_JSON) nvmeibt --userspace -o $(OBJ_DIR)/gen_events.h --only_trace_ids
# Compile traces_ids.c
	@printf "Making \e[0;32m traces_ids.o\e[0;0m\n"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $(OBJ_DIR)/traces_ids.c -o $(OBJ_DIR)/traces_ids.o
# Link everything
	@$(CC) $(LDFLAGS) -T$(TDIR)/tracer.ld $(OBJECTS) $(OBJ_DIR)/traces_ids.o -o $(EXE_NAME) $(LIBS_PATH) $(LIBS)
	@printf "Making \e[0;32m $@\e[0;0m done\n"

# Make each object depend on its corresponding gen_events header, This creates the dependency so headers are built before objects in parallel builds
define obj_dep_template
$(1): $(patsubst %.o,%.c_gen_events.h,$(1))
endef
$(foreach obj,$(OBJECTS),$(eval $(call obj_dep_template,$(obj))))

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "Compiling $< -> $@"
	@OBJ_REAL=$$(cd $(dir $@) && pwd)/$(notdir $@); \
	GEN_HEADER=$$(echo "$$OBJ_REAL" | sed 's/\.o$$/\.c_gen_events.h/'); \
	$(CC) $(CFLAGS) $(call per_file_cflags_varval,$<) $(INCLUDES) -include $$GEN_HEADER -c $< -MD -MP -o $@

define clean_compilation_intermediate
	@printf "+-->Cleaning recursive dir |\e[1;45m${1}/*/.[oda]\e[0;0m|\n"
	@find ./${1} -type f -name '*.[oda]' -delete
endef
define clean_prev_run
	@printf "+-->Cleaning previous run logs |\e[1;45mlogs\e[0;0m|\n"
	@rm -f ./_root/var/log/nvmesh/trace_daemon/toma.*;
	@rm -f ./_root/var/log/nvmesh/toma_2*;
	@rm -f ./_root/var/log/nvmesh/tracelist.txt;
	@rm -f ./_root/var/run/nvmesh/sandbox_*;
	@rm -f ./_root/etc/nvmesh/tls/*.[ct][rx]_
endef
clean:
	@printf "+-->Cleaning |\e[1;45mall\e[0;0m|\n"
	$(call clean_compilation_intermediate,$(OBJ_DIR_ROOT))
	@rm -f $(OBJECTS) $(BACKUPS) $(EXE_NAME) $(EXE_NAME).log $(EXE_NAME).err
	@rm -f $(AUTOGEN_TARGETS)
	@printf "+-->Removing |\e[1;45mgenerated trace files\e[0;0m|\n"
	@find $(OBJ_DIR_ROOT) -type f \( -name '*.trace.json' -o -name '*_gen_events.h' -o -name '*.i' \) -delete 2>/dev/null || true
	@rm -rf $(OBJ_DIR_ROOT)
	$(call clean_prev_run)

clean_run:
	$(call clean_prev_run)

# Prevent the deletion of our tracer intermediate files
.PRECIOUS: $(patsubst %.o,%.c_gen_events.h,$(OBJECTS)) $(patsubst %.o,%.trace.json,$(OBJECTS)) $(patsubst %.o,%.i,$(OBJECTS))

.PHONY: depend
-include $(DEPEND_FILES_ALL)
