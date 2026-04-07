# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# TOMA make file
# Supported command line options (the default is in ()):
# MOD=<(debug),release> for compilation mode
# DEBUG=<yes,(no)> if yes in release compilation it adds the TOMA_DEBUG define
#

$(info TOMA_MAKEFILE_START MOD=$(MOD))
$(info AUTOGEN_DIR is $(AUTOGEN_DIR))
$(info AUTOGEN_SUBDIRS_TOMA is $(AUTOGEN_SUBDIRS_TOMA))

# the supported compilation modes
DEBUG_STR=debug
RELEASE_STR=release
TOOLS_DIR=../tools

HIGH_LEVEL_DEBUG_FLAGS = -g3 -ggdb3       # Possibly add -Wall -fverbose-asm
CFLAGS_COMMON = $(HIGH_LEVEL_DEBUG_FLAGS)

# Set MOD to 'debug' or 'release'
ifeq ($(MOD),)
MOD=$(DEBUG_STR)
else ifeq ($(MOD),$(DEBUG_STR))
else ifeq ($(MOD),$(RELEASE_STR))
else
override MOD := $(DEBUG_STR)
endif

ifneq ($(MAKECMDGOALS),clean)
	ifeq ($(AUTOGEN_DIR),)
           $(error Autogen dir not specified. Try 'make AUTOGEN_DIR='path/to/autogen/dir'')
	endif
	ifeq ($(AUTOGEN_SUBDIRS_TOMA),)
           $(error Autogen subdirs for Toma not specified. Try 'make AUTOGEN_SUBDIRS_TOMA='common toma'')
	endif
endif
AUTOGEN_HEADERS := $(foreach dir,$(AUTOGEN_SUBDIRS_TOMA),$(AUTOGEN_DIR)/$(dir))

# Repo root (parent of this directory); override from top-level make via NVMESH_SRC_DIR=...
NVMESH_SRC_DIR ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

TRACER_PP  := $(NVMESH_SRC_DIR)/tools/pre_processor/tracer_pp.py
GEN_PROBES := $(NVMESH_SRC_DIR)/tools/pre_processor/gen_probes2.py
MERGE_MULTI_GEN := $(NVMESH_SRC_DIR)/tools/pre_processor/merge_multi_gen_events.py
MERGE_DICT := $(NVMESH_SRC_DIR)/tools/merge_dictionary.sh
DICT_JSON  := $(NVMESH_SRC_DIR)/tools/dictionary.json

EXTRA_SOURCES=$(TOOLS_DIR)/nvmeib_trace_userspace.o $(TOOLS_DIR)/nvmeib_trace_userspace_poller.o

# The programs to create
BINS = nvmeibt_toma

# the source directories, space separated
PROJECT_DIRS_nvmeibt_toma = . clnt ./local/ram ./interfaces/network ./interfaces/log ./interfaces/nvme ./interfaces/os ./interfaces/srvr ./utils ./vol ./unitest/00_framework/
# shared with kernel dirs...
SHARED_DIRS_nvmeibt_toma =
# files we would like to exclude from the above dir list
PROJECT_DIRS_EXCLUDES_nvmeibt_toma = ./interfaces/network/nvmeibt_nm_udp.c ./interfaces/network/nvmeibt_nm_ibud.c
# the binary compiler defines
COMPILER_DEFINES_nvmeibt_toma = TOMA TOMA_IB_ROCE
ifeq ($(MOD),$(DEBUG_STR))
COMPILER_DEFINES_nvmeibt_toma += TOMA_DEBUG
else ifeq ($(MOD),$(RELEASE_STR))
ifeq ($(DEBUG),yes)
COMPILER_DEFINES_nvmeibt_toma += TOMA_DEBUG
endif
endif
LINKER_CMDS_nvmeibt_toma = -T,tracer.ld

# the binary header dirs - these we add with the -I
HEADERS_nvmeibt_toma = rcvr /usr/include/infiniband $(AUTOGEN_HEADERS)
# the linker defines - currently only pthread but we may also e.g. static
LINKER_DEFINES_nvmeibt_toma = pthread
# external libraries we would link with
LDLIBS_DEFINES_nvmeibt_toma = pthread rt udev dl rdkafka m

ifeq ($(BUILD_ASAN),1)
	LDLIBS_DEFINES_nvmeibt_toma += asan
endif

ifeq ($(MEM),low)
COMPILER_DEFINES_nvmeibt_toma += LOW_MEM
endif

COMPILER_DEFINES_nvmeibt_toma += GIT_COMMIT_ID=\"$(GIT_COMMIT_ID)\" GIT_BRANCH=\"$(BRANCH_NAME)\" BUILD_VERSION_FOR_MGMT=\"$(VERSION)-$(RELEASE)\" BUILD_NUMBER_FOR_MGMT=\"$(BUILD_NUMBER)\"

# End of user stuff - below this line it is all internal.
# Changes below this comments are rarely needed so be sure
# you really understand what you are doing

ifeq ($(SILENT), yes)
TRACE=$(if $(VERBOSE),,@)
else
TRACE=$(if $(VERBOSE),@,)
endif
TRACE=$(if $(VERBOSE),@,)

# Architecture
ARCH:=$(shell /bin/uname -m)

# Set profiling...
NO_PROF = false
ifndef "PROF"
PROF = $(NO_PROF)
endif
ifneq ($(PROF),$(NO_PROF))
PROF_FLAGS = -pg
endif

# Set compiler to 'gnu' or ???
GNU_STR = gnu
COMP = $(GNU_STR)
CFLAGS_common_$(COMP) =
ifneq ($(LLVM),)
    CFLAGS_COMMON += -DLLVM -Wno-typedef-redefinition -Wno-missing-field-initializers -Wno-missing-braces -Wno-parentheses-equality -Wno-gnu-variable-sized-type-not-at-end
else
    CC=gcc
    CPP=g++
    LD=ld
    CFLAGS_COMMON += -Wno-old-style-declaration
    CFLAGS_common_$(COMP) += -rdynamic
endif
CFLAGS_common_$(COMP) += -DUSER_SPACE -DNVMEIBC_SECTOR_SHIFT=$(NVMEIBC_SECTOR_SHIFT)
CFLAGS_$(DEBUG_STR)_$(COMP) = $(CFLAGS_common_$(COMP))
CFLAGS_$(RELEASE_STR)_$(COMP) = $(CFLAGS_common_$(COMP)) -DNDEBUG -Og

ifeq ($(BUILD_ASAN),1)
	CFLAGS_COMMON += -fsanitize=address -fno-omit-frame-pointer
endif

# Dirs
SHARED_PATH=..
SHARED_NAME=common
DEBUG_RELEASE_STR=delease
TRAGET_ROOT = .
#BIN = $(TRAGET_ROOT)/$(ARCH)/bin/$(MOD)
BDIR = $(MOD)

ifeq ($(MOD),$(RELEASE_STR))
	ifeq ($(DEBUG),yes)
		BDIR = $(DEBUG_RELEASE_STR)
	endif
endif
BIN = $(TRAGET_ROOT)/bin/$(BDIR)
LIB_ROOT=lib
OBJ_ROOT=obj
I_ROOT=i
TRACE_AUTOGEN_ROOT=trace
PWD=$(shell pwd)

# Handle compiler stuff...
# Compiler flags...
ifeq ($(MOD),$(DEBUG_STR))
CFLAGS_$(DEBUG_STR) =
endif
ifeq ($(MOD),$(RELEASE_STR))
CFLAGS_$(RELEASE_STR) = $(PROF_FLAGS)
endif

CFLAGS_COMMON += -std=gnu99 -Wall -Werror -Wno-deprecated -Wdeclaration-after-statement -Wno-format-security -Wformat -Wshadow -Wstrict-prototypes -fstack-protector -pipe -I. -I$(SHARED_PATH)

# -Wextra and -Wstrict-aliasing (part of -Wall) causes compilation to fail on GCC 4.4.x
GCC_VER_GT44 := $(shell echo `gcc -dumpversion | cut -f1-2 -d.` \> 4.4 | bc )
ifeq ($(GCC_VER_GT44),1)
CFLAGS_COMMON += -Wextra
else
CFLAGS_COMMON += -Wno-strict-aliasing
endif

#nvme.h/nvme_ioct.h location under /usr/include/ compatibility
UINC := /usr/include
ifneq ($(wildcard $(UINC)/linux/nvme.h),)
	CFLAGS_COMMON += -DUINC_LINUX_NVME=1
endif

ifneq ($(wildcard $(UINC)/linux/nvme_ioctl.h),)
	CFLAGS_COMMON += -DUINC_LINUX_NVME_IOCTL=1
endif

ifneq ($(wildcard $(UINC)/uapi/nvme.h),)
	CFLAGS_COMMON += -DUINC_UAPI_NVME=1
endif

ifneq ($(wildcard $(UINC)/uapi/nvme_ioctl.h),)
	CFLAGS_COMMON += -DUINC_UAPI_NVME_IOCTL=1
endif

#nvme_opcode
NVME_PATH = \
	$(UINC)/linux/nvme.h \
	$(UINC)/linux/nvme_ioctl.h \
	$(UINC)/uapi/nvme.h \
	$(UINC)/uapi/nvme_ioctl.h
ifeq ($(shell grep nvme_opcode -- $(NVME_PATH) 2> /dev/null),)
    cflags += -DUINC_NVME_OPCODE_DEFINED=0
else
    cflags += -DUINC_NVME_OPCODE_DEFINED=1
endif

# ifneq ($(shell grep -so complib_init_v2 /usr/include/infiniband/complib/cl_debug.h),)
# 	CFLAGS_COMMON += -DCOMPLIB_INIT_V2
# else ifneq ($(shell grep -so complib_init /usr/include/infiniband/complib/cl_debug.h),)
# 	CFLAGS_COMMON += -DCOMPLIB_INIT
# else ifeq ($(UDPONLY),yes)
# 	# nothing needed in udp-only mode
# else
#     $(error Couldn't detect which opensm complib_init() to use)
# endif

# add -Wno-unused-result only on gcc >= 4.4
ifeq ($(GCC_MAJOR),4)
GCCGREATERTHEN44 := $(shell expr `gcc -dumpversion | cut -f2 -d.` \>= 4)
else
GCCGREATERTHEN44 := $(shell expr `gcc -dumpversion | cut -f1 -d.` \>= 5)
endif
ifeq ($(GCCGREATERTHEN44),1)
CFLAGS_COMMON += -Wno-unused-result -Wno-address-of-packed-member
endif
# CPPFLAGS_COMMON = -Wall -Werror -pipe -I. $(HIGH_LEVEL_DEBUG_FLAGS)
CFLAGS_MOD = $(CFLAGS_$(MOD)_$(COMP)) $(CFLAGS_$(MOD))
CFLAGS_x86_64 = -m64
CFLAGS_ALL = $(CFLAGS_COMMON) $(CFLAGS_$(ARCH)) $(CFLAGS_MOD)

# Compile templates
define CFLAGS_template
	CFLAGS_$1 = $$(foreach flag,$$(COMPILER_DEFINES_$1), $$(addprefix -D,$$(flag))) $$(CFLAGS_ALL)
	CFLAGS_$1 += $$(foreach header,$$(HEADERS_$1), $$(addprefix -I,$$(header)))
endef

# Generate CFLAGS var per binary...
$(foreach bin,$(BINS),$(eval $(call CFLAGS_template,$(bin))))

# Handle linker stuff...
# Linker flags...
LDFLAGS_x86_64=-m64 -rdynamic $(HIGH_LEVEL_DEBUG_FLAGS)
LDFLAGS_aarch64=-rdynamic

# Linker templates
define LDFLAGS_template
	LDFLAGS_$1 = $$(foreach flag,$$(LINKER_DEFINES_$1), $$(addprefix -,$$(flag)))
endef

# Generate LDFLAGS var per binary...
$(foreach bin,$(BINS),$(eval $(call LDFLAGS_template,$(bin))))

# Linker commands templates
define LINKER_PASS_template
	_LINKER_PASS_$1 = $$(foreach flag,$$(LINKER_CMDS_$1), $$(addprefix ,,$$(flag)))
	ifeq ($$(_LINKER_PASS_$1),)
	LINKER_PASS_$1 =
	else
	LINKER_PASS_$1 = $$(addprefix -Wl,$$(_LINKER_PASS_$1))
	endif
endef

# Generate LDFLAGS var per binary...
$(foreach bin,$(BINS),$(eval $(call LINKER_PASS_template,$(bin))))

# Handle library stuff...
# Libs...
define LDLIBS_template
	LDLIBS_$1 = $$(foreach lib,$$(LDLIBS_DEFINES_$1), $$(addprefix -l,$$(lib)))
endef

# Generate LDLIBS var per binary...
$(foreach bin,$(BINS),$(eval $(call LDLIBS_template,$(bin))))

# Operators
alldirtype = $(call addobj,$(patsubst %.$2,%.o,$(filter-out $1/test%.$2,$(wildcard $1/*.$2))),$3)
alldir = $(foreach dir,$1,$(call alldirtype,$(dir),c,$2) $(call alldirtype,$(dir),cpp,$2))
deldir = $(patsubst $1/%,%,$2)
adddir = $(addprefix $1/,$(call deldir,$1,$2))
addobj = $(call adddir,$2,$1)
addbin = $(call adddir,$(BIN),$1)
addlib = $(call adddir,$(LIB),$1)
addtest = $(call adddir,$(TST),$1)
rmbin = $(call deldir,$(BIN),$1)
rmobj = $(call deldir,$(OBJ),$1)
rmlib = $(call deldir,$(LIB),$1)
rmtest = $(call deldir,$(TST),$1)
info = $(if $(VERBOSE),,@echo $1)
unobj = $(call deldir,$(OBJ),$1)
unlib = $(call deldir,$(LIB),$1)
addlibs = $(call addlib,$(patsubst %,lib%.a,$1))
info = $(if $(VERBOSE),,@echo $1)

allsdirtype=$(call addobj,$(patsubst $(SHARED_PATH)/%.$2,%.o,$(filter-out $(SHARED_PATH)/$1/test%.$2,$(wildcard $(SHARED_PATH)/$1/*.$2))),$3)
allsdir=$(foreach dir,$(call adddir,$(SHARED_NAME),$1),$(call allsdirtype,$(dir),c,$2) $(call allsdirtype,$(dir),cpp,$2))

allexclude = $(filter-out $(foreach file,$1,$(call addobj,$(patsubst %.cpp,%.o,$(patsubst %.c,%.o,$(file))),$2)),$3)
allinclude = $(foreach file,$1,$(call addobj,$(patsubst %.cpp,%.o,$(patsubst %.c,%.o,$(file))),$2))

autodepend=-MT "$(OBJ)/$*.o" -MMD -MP -MF $(OBJ)/$*.d
ifneq ($(LLVM),)
    autocompdb=-MJ $(OBJ)/$*.o.json
    COMPILEDB=../build_compile_db.sh
endif
dfile=$(OBJ)/$*.d

ALL_MAKEFILES := Makefile

# targets...
# the + is to force the make even if we call make -n which means just print
all: FORCE
	@# we can generated files make command here
	@# and the real command to make all binaries
	+$(MAKE) $(call addbin, $(call rmbin, $(BINS)))

# The lines below are used to generate unique macro __FILE_LITERAL__
# for each source file that evaluates to file_name_c without "
define get_file_literal
-D__FILE_LITERAL__=$(basename $(notdir $1))_c
endef

UTILS_DIR := ../utils
JDR_DIR := $(UTILS_DIR)/nvmeib_jdr
JDR_BUILD_DIR := $(JDR_DIR)/build
JDR_STATIC_LIB := libnvmeib_jdr.a
JDR_STATIC_LIB_PATH := $(JDR_BUILD_DIR)/$(JDR_STATIC_LIB)

# Build nvmeib_jdr static library by invoking its own Makefile
$(JDR_STATIC_LIB_PATH):
	$(MAKE) -C $(JDR_DIR)

# this is the main BIN generation template...
define BIN_template
# Important: = not :=
$(BIN)/$1: CFLAGS=$$(call get_file_literal,$$<)
$(BIN)/$1: CFLAGS+=$$(CFLAGS_$1)
$(BIN)/$1: CFLAGS+=-I$(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/
$(BIN)/$1: LDFLAGS=$$(LDFLAGS_$1) $(PROF_FLAGS)
$(BIN)/$1: LDLIBS=$$(LDLIBS_$1)

$(BIN)/$1: LINKER_PASS=$$(LINKER_PASS_$1)
GEN_EVENTS_$1 := $(shell pwd)/$(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/gen_events.h
TRACE_JSON_$1 := $(shell pwd)/$(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/nvmeibt.trace.json
ID_TRACES_C_$1 := $(shell pwd)/$(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/traces_ids.c
ID_TRACES_O_$1 := $(shell pwd)/$(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/traces_ids.o
PWD := $(shell pwd)

$(BIN)/$1: OBJ=$(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)
$(BIN)/$1: I_PATH=$(TRAGET_ROOT)/$(I_ROOT)/$1/$(BDIR)

$(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)/%.o: %.c $(ALL_MAKEFILES) $(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/%.c_gen_events.h
	@mkdir -p $$(dir $$@)
	@echo $(TRACE)$(CC) -D_GNU_SOURCE -c $$(CFLAGS) $$(autodepend) $$(autocompdb) -include $(PWD)/$(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/$$<_gen_events.h -o $$@ $$<
	$(TRACE)$(CC) -D_GNU_SOURCE -c $$(CFLAGS) $$(autodepend) $$(autocompdb) -include $(PWD)/$(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/$$<_gen_events.h -o $$@ $$<
$(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)/%.o: $(SHARED_PATH)/%.c $(ALL_MAKEFILES) $(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/%.c_gen_events.h
	@mkdir -p $$(dir $$@)
	@echo $(TRACE)$(CC) -D_GNU_SOURCE -c $$(CFLAGS) $$(autodepend) $$(autocompdb) -include $(PWD)/$(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/$$*.c_gen_events.h -o $$@ $$<
	$(TRACE)$(CC) -D_GNU_SOURCE -c $$(CFLAGS) $$(autodepend) $$(autocompdb) -include $(PWD)/$(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/$$*.c_gen_events.h -o $$@ $$<
$(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)/%.o: %.cpp $(ALL_MAKEFILES)
	@mkdir -p $$(dir $$@)
	@echo $(TRACE)$(CPP) -c $$(CFLAGS) $$(autodepend) $$(autocompdb) -o $$@ $$<
	$(TRACE)$(CPP) -c $$(CFLAGS) $$(autodepend) $$(autocompdb) -o $$@ $$<
$(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)/%.o: $(SHARED_PATH)/%.cpp $(ALL_MAKEFILES)
	@mkdir -p $$(dir $$@)
	@echo $(TRACE)$(CPP) -c $$(CFLAGS) $$(autodepend) $$(autocompdb) -o $$@ $$<
	$(TRACE)$(CPP) -c $$(CFLAGS) $$(autodepend) $$(autocompdb) -o $$@ $$<
$(TOOLS_DIR)/%.o: $(TOOLS_DIR)/%.c
	@echo $(TRACE)$(CC) -c $$(CFLAGS) -o $$@ $$<
	$(TRACE)$(CC) -c $$(CFLAGS) -o $$@ $$<

$(BIN)/nm_udp.so: $(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/interfaces/network/nvmeibt_nm_udp.c_gen_events.h
$(BIN)/nm_ibud.so: $(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/interfaces/network/nvmeibt_nm_ibud.c_gen_events.h
$(BIN)/$1: $(BIN)/nm_udp.so $(BIN)/nm_ibud.so $$(call allexclude,$(PROJECT_DIRS_INCLUDES_$1),$(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR),$(call allexclude,$(PROJECT_DIRS_EXCLUDES_$1),$(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR),$(call alldir, $(PROJECT_DIRS_$1),$(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)) $(call allsdir, $(SHARED_DIRS_$1),$(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)))) $$(call allinclude,$(PROJECT_DIRS_INCLUDES_$1),$(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)) $(JDR_STATIC_LIB_PATH) $(EXTRA_SOURCES)
	# first we merge all the jsons
	$(MERGE_MULTI_GEN) $$(TRACE_JSON_$1) $(shell pwd)/$(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR) $$(filter-out %.so,$$^) $(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)/nvmeibt_nm_ibud.o $(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)/nvmeibt_nm_udp.o
	# now we will compile the extern id traces file
	$(GEN_PROBES) $$(TRACE_JSON_$1) nvmeibt --userspace -o $$(GEN_EVENTS_$1) --only_trace_ids
	@echo $(CC) -c -o $$(ID_TRACES_O_$1) $$(ID_TRACES_C_$1)
	$(CC) -c -o $$(ID_TRACES_O_$1) $$(ID_TRACES_C_$1)
	@mkdir -p $(BIN)
	# create the shared libraries
	$(TRACE)$(CC) -D_GNU_SOURCE -c $$(filter-out -D__FILE_LITERAL__%,$$(CFLAGS)) -D__FILE_LITERAL__=nvmeibt_nm_udp_c -fpic -include $(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/interfaces/network/nvmeibt_nm_udp.c_gen_events.h -o $(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)/nvmeibt_nm_udp.o $(TRAGET_ROOT)/interfaces/network/nvmeibt_nm_udp.c
	$(TRACE)$(CC) -shared -Ttracer.ld -o $(BIN)/nw_udp.so $(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)/nvmeibt_nm_udp.o
	$(TRACE)$(CC) -D_GNU_SOURCE -c $$(filter-out -D__FILE_LITERAL__%,$$(CFLAGS)) -D__FILE_LITERAL__=nvmeibt_nm_ibud_c -fpic -include $(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/interfaces/network/nvmeibt_nm_ibud.c_gen_events.h -o $(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)/nvmeibt_nm_ibud.o $(TRAGET_ROOT)/interfaces/network/nvmeibt_nm_ibud.c
	$(TRACE)$(CC) -shared -Wl,--no-as-needed -libverbs -lrdmacm -Ttracer.ld -o $(BIN)/nw_ibud.so  $(TRAGET_ROOT)/$(OBJ_ROOT)/$1/$(BDIR)/nvmeibt_nm_ibud.o
	# now we can safly link
	@echo $(TRACE)$(CC) $$(LDFLAGS) $(LDFLAGS_$(ARCH)) -o $$@ $$(filter-out %.so,$$^) $$(ID_TRACES_O_$1)  $$(LDLIBS) $$(LINKER_PASS)
	$(TRACE)$(CC) $$(LDFLAGS) $(LDFLAGS_$(ARCH)) -o $$@ $$(filter-out %.so,$$^) $$(JDR_STATIC_LIB_PATH) $$(ID_TRACES_O_$1)  $$(LDLIBS) $$(LINKER_PASS)
ifeq ($(MOD),$(RELEASE_STR))
	cp $$@ $$@.with_symbols
	## One solution is to create a symbols_only file
	#	objcopy --only-keep-debug $$@ $$@.symbols_only
	#	objcopy --add-gnu-debuglink=$$@.symbols_only $$@	# Add a debug link to allow GDB to find the file with the debugging symbols.
	#	gpg -c -z6 --batch --pinentry-mode default --passphrase nvmesh1111 < $$@.symbols_only > $$@.symbols_only.gpg
	## A second solution is not to strip since it is an internal (NVIDIA) distribution
	#	strip $$@
	(cd ..; tar -cf - `find ./toma ./common -name \*.[ch]` toma/bin/release/nvmeibt_toma | gpg -c -z6 --batch --pinentry-mode default --passphrase AlexanderRonen) > $$@_src_tar.pgp
endif
	flock $(BIN) cp -f $$(TRACE_JSON_$1) $(BIN)/
	$(COMPILEDB)
	@echo done making $$@

# Traces
$(TRAGET_ROOT)/$(I_ROOT)/$1/$(BDIR)/%.i: %.c $(ALL_MAKEFILES)
	@mkdir -p $$(dir $$@)
	$(TRACE)$(CC) $$(CFLAGS) -E -D__FIRST_PASS__ -D"__attribute__(x)=" -D_NVMEIB_TRACE_BACKEND_USER -o $$@ $$<

PREPROCESSED_$1 =  $(patsubst %.c,$(TRAGET_ROOT)/$(I_ROOT)/$1/$(BDIR)/%.i,$(shell find . -name '*.c'))
PREPROCESSED_$1 += $(patsubst %.c,$(TRAGET_ROOT)/$(I_ROOT)/$1/$(BDIR)/%.i,$(PROJECT_DIRS_INCLUDES_$1))

$(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/%.trace.json: $(TRAGET_ROOT)/$(I_ROOT)/$1/$(BDIR)/%.i $(MERGE_DICT) $(ALL_MAKEFILES) $(TRACER_PP)
	@mkdir -p $$(dir $$@)
	$(TRACE)$(TRACER_PP) -j 1 $$@ $$<
	$(TRACE)$(MERGE_DICT) $$@ $(DICT_JSON)

$(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/%.c_gen_events.h: $(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/%.trace.json $(GEN_PROBES) Makefile
	@mkdir -p $$(dir $$@)
	cd $$(dir $$@); $(GEN_PROBES) $(PWD)/$$<  nvmeibt --userspace -o $(PWD)/$$@

# prevent the deltion of our tracer intermidate files
.PRECIOUS: $(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/%.c_gen_events.h $(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT)/$1/$(BDIR)/%.trace.json $(TRAGET_ROOT)/$(I_ROOT)/$1/$(BDIR)/%.i
endef
define BINS_template
$1: FORCE
	@# we can generated files make command here
	@# and the real command to make the specific binary
	+$(MAKE) $(BIN)/$1
endef

# Generate BIN target with per-BIN specific vars...
$(foreach bin,$(BINS),$(eval $(call BIN_template,$(bin))))
$(foreach bin,$(BINS),$(eval $(call BINS_template,$(bin))))

clean:
	@rm -rf $(wildcard $(foreach bin,$(BINS),$(TRAGET_ROOT)/$(OBJ_ROOT)/$(bin)/$(BDIR)) $(foreach bin,$(BINS),$(BIN)/$(bin))) $(TOOLS_DIR)/*.o $(patsubst %.c,%.i,$(shell find . -name '*.c')) $(shell find . -name 'dict.*.json')
	@rm -rf $(TRAGET_ROOT)/$(TRACE_AUTOGEN_ROOT) $(TRAGET_ROOT)/$(I_ROOT)

.PHONY: clean $(rmbin $(BINS))

FORCE:

# Autodepend
-include $(foreach bin,$(BINS),$(shell find $(TRAGET_ROOT)/$(OBJ_ROOT)/$(bin)/$(BDIR) -name '*.d' 2> /dev/null))


