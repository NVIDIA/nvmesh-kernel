# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

.SUFFIXES:
.PHONY: clean all
.DEFAULT_GOAL := all
SSDA?=../../..

CFLAGS  :=  -DFORMATTERS_APP -ggdb3 -Wall -Werror -fPIC -DNVMEIBC_SECTOR_SHIFT=12 -Wno-address-of-packed-member -DUSER_SPACE -DPAGER_FORMATTER_FUNCTIONS=1

LDFLAGS := -shared
IFLAGS  := -I$(SSDA)/ -I$(SSDA)/clnt/ -I$(SSDA)/srv/ -I$(SSDA)/common/ \
	-I$(SSDA)/common_public/ -I$(SSDA)/toma/ -I$(SSDA)/autogen/common/ \
	-I$(SSDA)/autogen/clnt/ $(FORMATTERS_EXTENSION_I)

BUILDDIR  := ./build
CC      := gcc
LD      := $(CC)
#TRACE    = @echo '>' $@ ;
DEPEND   = -MT $*.o -MMD -MP -MF $*.d

ifneq ($(FORMATTERS_EXTENSION_O),)
  _FORMATTERS_EXTENSION_O = $(BUILDDIR)/$(FORMATTERS_EXTENSION_O)
else
  _FORMATTERS_EXTENSION_O =
endif

OBJ := \
	$(BUILDDIR)/formatter_functions.o \
	$(BUILDDIR)/$(SSDA)/clnt/block/datapath_utils_generic/nvmeibc_block_dp_block_md_tables.o \
	$(_FORMATTERS_EXTENSION_O)

DEP := $(OBJ:.o=.d)
LIB := libfmtrs.so

-include $(DEP)

all: $(LIB)

# test: test.o $(LIB)
# 	$(TRACE)$(LD) test.o -ldl -o $@

$(LIB): $(OBJ)
	$(TRACE)$(LD) $(LDFLAGS) $(OBJ) -o $@

$(BUILDDIR)/%.o: %.c Makefile
	$(TRACE) mkdir -p $(shell dirname "$@")
	$(TRACE)$(CC) $(CFLAGS) $(IFLAGS) $(DEPEND) -o $@ -c $<

clean:
	$(TRACE)rm -rf test $(LIB) $(OBJ) $(DEP)
