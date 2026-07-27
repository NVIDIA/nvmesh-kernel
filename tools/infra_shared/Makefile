.SUFFIXES:
.PHONY: clean all
.DEFAULT_GOAL := all

CFLAGS := -ggdb3 -Wall -Werror -fPIC -DNVMEIBC_SECTOR_SHIFT=12 -Wno-address-of-packed-member -O3 -DUSER_SPACE

ifeq ($(shell uname -m),aarch64)
CFLAGS += -march=armv8.1-a+crc+fp+simd
endif

SSDA?=../..
LDFLAGS := -shared
IFLAGS  := -I$(SSDA)/ -I$(SSDA)/common/ -I$(SSDA)/common_public/ -I$(SSDA)/clnt/
CC      := gcc
LD      := $(CC)
TRACE    = @echo '>' $@ ;
BUILD_DIR   = $(SSDA)/build/
DEPEND   = -MT $(BUILD_DIR)$*.o -MMD -MP -MF $(BUILD_DIR)$*.d
SRCS := tools/infra_shared/infra_shared.c clnt/block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_log.c
OBJS := $(addprefix $(BUILD_DIR), $(SRCS:.c=.o))
DEP := $(OBJS:.o=.d)
LIB := infra_shared.so
EXE := infra_shared_test

-include $(BUILD_DIR)$(DEP)

all: $(EXE)

$(LIB): $(OBJS)
	$(TRACE)$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD_DIR)%.o: $(SSDA)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(IFLAGS) $(DEPEND) -c $< -o $@

$(BUILD_DIR)tools/infra_shared/$(EXE).o: $(EXE).c
	$(CC) $(CFLAGS) $(IFLAGS) $(DEPEND) -c $< -o $@

$(EXE): $(LIB) $(BUILD_DIR)tools/infra_shared/$(EXE).o
	$(CC) $(CFLAGS) $(LIB)  $(BUILD_DIR)tools/infra_shared/$(EXE).o -o $(EXE) -L. -ldl ./$(LIB)

clean:
	$(TRACE)rm -rf $(LIB) $(BUILD_DIR) $(DEP) $(EXE)
