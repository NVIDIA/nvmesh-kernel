.SUFFIXES:
.PHONY: clean all
.DEFAULT_GOAL := all

CFLAGS  := -ggdb3 -Wall -Werror -D_GNU_SOURCE
LDFLAGS := -pthread
CC      := gcc
LD      := $(CC)
TRACE    =#@echo '>' $@ ;
DEPEND   = -MT $*.o -MMD -MP -MF $*.d
NVMESH_SRC_DIR ?= ../..

EXE := trace_daemon
OBJ := main.o trace_channel.o mmap_manager.o capuch_worker.o unlink_list.o
OBJ := $(foreach obj,$(OBJ),$(shell realpath $(obj)))
TOOLS_LIB := -I$(NVMESH_SRC_DIR)/tools
STATIC_LIBS := $(NVMESH_SRC_DIR)/tools/trace_compress_lib/libcompress.a  $(NVMESH_SRC_DIR)/tools/lz4/lib/liblz4.a

DEP := $(OBJ:.o=.d)

-include $(DEP)

all: $(EXE)

$(EXE): $(OBJ)
	$(TRACE)$(LD) $(LDFLAGS) $(OBJ) $(STATIC_LIBS) -o $@

%.o: %.c Makefile
	$(TRACE)$(CC) $(CFLAGS) $(TOOLS_LIB) $(DEPEND) -o $@ -c $<

clean:
	$(TRACE)rm -rf $(EXE) $(OBJ) $(DEP)
