BUILD_DIR := build
TESTS_DIR := tests
SHELL := /bin/bash
NVMESH_SRC_DIR ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

#the developer is expected to install it
KSC = kaitai-struct-compiler

# Input and output files
PET_SPEC_FILE = nvmeib_pet_specification.ksy
PET_BINFILE_READER = nvmeib_pet_archive.py
GIT_COMMIT_ID = $(shell COMMIT_ID=; source ../../build_common.sh; git_commit_id)

ifeq ($(filter-out 0x,$(COMMIT_ID)),)
override COMMIT_ID = 0x$(GIT_COMMIT_ID)
endif

ifeq ($(filter-out 0x,$(COMMIT_ID_STR)),)
override COMMIT_ID_STR = $(GIT_COMMIT_ID)
endif

CC := gcc
OPTFLAGS := -O0
ifneq ($(filter perf,$(MAKECMDGOALS)),)
OPTFLAGS := -O2
endif
CFLAGS := -Wall -Wextra -ggdb $(OPTFLAGS) -I"." -I".." -I"../.." -DCOMMIT_ID=$(COMMIT_ID) -DCOMMIT_ID_STR=\"$(COMMIT_ID_STR)\"
CFLAGS_STAMP := $(BUILD_DIR)/.test.cflags

PERF ?= perf
PERF_DATA ?= $(BUILD_DIR)/perf.data
PERF_CALL_GRAPH ?= dwarf
PERF_RECORD_FLAGS ?= -F 999 -g --call-graph $(PERF_CALL_GRAPH)
PERF_REPORT_FLAGS ?= --stdio --stdio --sort=symbol --percentage=absolute -g none

TEST_EXE := $(BUILD_DIR)/test
TEST_OBJ := $(BUILD_DIR)/test.o
TEST_DCT := $(BUILD_DIR)/dict.${COMMIT_ID}.json
TEST_OUT := $(BUILD_DIR)/messages.pet

.PHONY: all clean test check dirs demo demo_dict perf fast_build FORCE

# Default target: build generated sources and test binary.
all: $(PET_BINFILE_READER) dirs $(TEST_EXE)

$(PET_BINFILE_READER): $(PET_SPEC_FILE)
	$(KSC) --target python $(PET_SPEC_FILE)

dirs:
	@mkdir -p $(BUILD_DIR)

$(CFLAGS_STAMP): FORCE | dirs
	@printf '%s\n' '$(CFLAGS)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm $@.tmp; fi

$(BUILD_DIR)/test.o: $(TESTS_DIR)/test.c nvmeib_pet_specification.h $(CFLAGS_STAMP) | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_EXE): $(TEST_OBJ)
	$(CC) $^ -o $@

test: dirs $(TEST_EXE)
	./$(TEST_EXE) $(TEST_OUT)

check: test

demo_dict: dirs test $(PET_BINFILE_READER)
	PY=$(PY) ./pet_messages.sh save-dictionary $(TEST_EXE) test_pet_msgs $(TEST_DCT)

demo: dirs test demo_dict $(PET_BINFILE_READER)
	PY=$(PY) ./pet_messages.sh view --no-sort $(BUILD_DIR) $(TEST_OUT)

perf: $(TEST_EXE)
	$(PERF) record $(PERF_RECORD_FLAGS) -o $(PERF_DATA) -- $(TEST_EXE)
	$(PERF) report $(PERF_REPORT_FLAGS) -i $(PERF_DATA) 2>&1 | rg -o "^\s+[0-9].*pet[^\s]*" | head -40

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(PET_BINFILE_READER)

fast_build: dirs $(TEST_EXE) test
