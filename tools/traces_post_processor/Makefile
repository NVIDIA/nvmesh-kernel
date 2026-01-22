# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

.SUFFIXES:
.PHONY: clean all rebuild
.DEFAULT_GOAL := all

NVMESH_SRC_DIR ?= ../..

ARCH	:= $(shell uname -m)
CFLAGS   := -Wall -ggdb -O3 -Wno-strict-aliasing -Wno-format
INCLUDES := -I$(NVMESH_SRC_DIR)/tools/ -I.
LDFLAGS  :=
COMPRESS_LIBS := $(NVMESH_SRC_DIR)/tools/trace_compress_lib/libcompress.a  $(NVMESH_SRC_DIR)/tools/lz4/lib/liblz4.a
LDSTATIC := $(COMPRESS_LIBS) ./json-c/lib/libjson-c.a.lib.$(ARCH)
LDDYN    := -ljson-c
CFLAGS   += -DPAGER_APP -DNVMEIB_COMPILATION_DATE=NVMEIB_DATE\($(shell date +%-d,%-m,%Y)\)# Compilation date in form of macro
EXEC_NAME = cpager

has_pie := $(shell gcc -v 2>&1 | grep pie > /dev/null && echo 1 || echo 0)
ifeq ($(has_pie),1)
  LDSTATIC += -no-pie
  LDFLAGS += -no-pie
endif

PAGER_OBJ := \
	pager.o \
	pager_infra.o \
	pager_buf_stream.o \
	pager_msg_stream.o \
	binary_heap.o \
	formatter.o \
	pager_filter.o \
	pager_serialized_msg_stream.o

# These objects are from auto-generated source files
GRAMR_OBJ := \
	pager_filter.bison.o \
	pager_filter.flex.o

ifeq ($(USE_MUSL),1)
	CC=musl-gcc
	LDFLAGS += -static
else
  ifeq ($(USE_CLANG),1)
    CC=clang
    CFLAGS += -Wno-format-security -Wtype-safety
  else
    CC=gcc
  endif
endif

COMPILE     := $(CC) $(CFLAGS) $(INCLUDES) -MD -MP -c
LINK        := $(CC) $(CFLAGS) $(LDFLAGS)

ifeq ($(PAGER_DYNAMIC_LINK),1)
  LINK_PAGER = $(LINK) $(PAGER_OBJ) $(GRAMR_OBJ) $(LDDYN)
else
  LINK_PAGER = $(LINK) $(PAGER_OBJ) $(GRAMR_OBJ) $(LDSTATIC)
endif

ifeq ($(MAKECMDGOALS),grammar_src)

%.bison.c: %.y
	@echo '>o>o>' $@
	@bison -d $(shell readlink -m $*.y) -Wno-conflicts-sr -o $@

%.flex.c: %.l %.bison.c
	@echo '>o>o>' $@
	@flex -o $@ $(shell readlink -m $*.l)

endif

%.o: %.c Makefile
	@echo '>o>o>' $@
	@$(COMPILE) $(shell readlink -m $<) -o $@

-include $(PAGER_OBJ:.o=.d) $(GRAMR_OBJ:.o=.d)

# Build dependency libraries if not already built.
# This allows standalone invocation (e.g. from toma/unitest) without requiring
# the caller to know cpager's dependency chain.
$(NVMESH_SRC_DIR)/tools/lz4/lib/liblz4.a:
	$(MAKE) -C $(NVMESH_SRC_DIR)/tools/lz4/lib liblz4.a

$(NVMESH_SRC_DIR)/tools/trace_compress_lib/libcompress.a:
	$(MAKE) NVMESH_SRC_DIR=$(abspath $(NVMESH_SRC_DIR)) \
	  -C $(NVMESH_SRC_DIR)/tools/trace_compress_lib

# -ldl should come last to resolve symbols correctly on some systems
$(EXEC_NAME): $(PAGER_OBJ) $(GRAMR_OBJ) $(COMPRESS_LIBS)
	@echo '>o>o>' $(EXEC_NAME)
	@$(LINK_PAGER) -o $(EXEC_NAME) -ldl

# Backward-compatible alias (top-level Makefile invokes 'make pager')
pager: $(EXEC_NAME)

all: pager

clean:
	@rm -f $(PAGER_OBJ) $(GRAMR_OBJ)
	@rm -f $(PAGER_OBJ:.o=.d) $(GRAMR_OBJ:.o=.d)
	@rm -f merge_trace $(EXEC_NAME)

grammar_src: $(GRAMR_OBJ:.o=.c)

rebuild: clean
	$(MAKE)
