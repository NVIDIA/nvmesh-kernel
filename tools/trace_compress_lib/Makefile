.SUFFIXES:
.PHONY: clean all
.DEFAULT_GOAL := all

CFLAGS  := -O3 -Wall -Werror
CC      := gcc
LD      := $(CC)
TRACE    =#@echo '>' $@ ;
DEPEND   = -MT $*.o -MMD -MP -MF $*.d

NVMESH_SRC_DIR ?= ../..
OBJ := compressor.o
TEST_OBJ := test.o
OBJ := $(foreach obj,$(OBJ),$(shell realpath $(obj)))
COMPRESS_LIB := libcompress.a
LZA_INCLUDE := -I$(NVMESH_SRC_DIR)/tools/lz4
STATIC_LIBS := $(NVMESH_SRC_DIR)/tools/lz4/lib/liblz4.a
TEST_EXE := ./test_lz4

DEP := $(OBJ:.o=.d)

-include $(DEP)

all: $(COMPRESS_LIB)

test: $(TEST_OBJ) $(OBJ)
	$(TRACE)$(LD) $(TEST_OBJ) $(OBJ) $(STATIC_LIBS) -o $(TEST_EXE)
	@echo "executing the test program."
	$(TEST_EXE)

$(COMPRESS_LIB): $(OBJ)
	echo "Creating static library $@"
	$(TRACE)$(AR) rcs $@ $^

%.o: %.c Makefile
	$(TRACE)$(CC) $(CFLAGS) $(LZA_INCLUDE) $(DEPEND) -o $@ -c $<

clean:
	$(TRACE)rm -rf $(OBJ) $(DEP) $(TEST_EXE) $(COMPRESS_LIB)
