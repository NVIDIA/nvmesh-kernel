.SUFFIXES:
.PHONY: clean all
.DEFAULT_GOAL := all

CFLAGS  := -ggdb3 -Wall -Werror -D_GNU_SOURCE
LDFLAGS := -pthread
TEST_CFLAGS := $(CFLAGS) -fsanitize=address -fsanitize=undefined
TEST_LDFLAGS := $(LDFLAGS) -fsanitize=address -fsanitize=undefined
CC      := gcc
LD      := $(CC)
TRACE    =#@echo '>' $@ ;
DEPEND   = -MT $*.o -MMD -MP -MF $*.d
NVMESH_SRC_DIR ?= ../..

EXE := trace_daemon
TEST_EXE := unlink_list_test
OBJ := main.o trace_channel.o mmap_manager.o capuch_worker.o unlink_list.o io_pet_channel.o
OBJ := $(foreach obj,$(OBJ),$(shell realpath $(obj)))
TEST_OBJ := unlink_list_test.o unlink_list_test_impl.o
TOOLS_LIB := -I$(NVMESH_SRC_DIR)/tools
STATIC_LIBS := $(NVMESH_SRC_DIR)/tools/trace_compress_lib/libcompress.a  $(NVMESH_SRC_DIR)/tools/lz4/lib/liblz4.a

DEP := $(OBJ:.o=.d) $(TEST_OBJ:.o=.d)

-include $(DEP)

all: $(EXE) $(TEST_EXE)

test: $(TEST_EXE)

$(EXE): $(OBJ)
	$(TRACE)$(LD) $(LDFLAGS) $(OBJ) $(STATIC_LIBS) -o $@

$(TEST_EXE): $(TEST_OBJ)
	$(TRACE)$(LD) $(TEST_LDFLAGS) $(TEST_OBJ) -o $@

unlink_list_test.o: unlink_list_test.c Makefile
	$(TRACE)$(CC) $(TEST_CFLAGS) $(TOOLS_LIB) $(DEPEND) -o $@ -c $<

unlink_list_test_impl.o: unlink_list.c Makefile
	$(TRACE)$(CC) $(TEST_CFLAGS) $(TOOLS_LIB) $(DEPEND) -o $@ -c $<

%.o: %.c Makefile
	$(TRACE)$(CC) $(CFLAGS) $(TOOLS_LIB) $(DEPEND) -o $@ -c $<

clean:
	$(TRACE)rm -rf $(EXE) $(TEST_EXE) $(OBJ) $(TEST_OBJ) $(DEP)
