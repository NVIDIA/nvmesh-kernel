# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

BUILD_DIR := build
TESTS_DIR := tests
ROTATIONS_DIR := $(BUILD_DIR)/rotations
SHELL := /bin/bash
NVMESH_SRC_DIR ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

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
CFLAGS := -std=gnu99 -Wall -Wextra -ggdb $(OPTFLAGS) -I"." -I".." -I"../.." -DCOMMIT_ID=$(COMMIT_ID) -DCOMMIT_ID_STR=\"$(COMMIT_ID_STR)\"
LDFLAGS :=
ifeq ($(COVERAGE),1)
CFLAGS += --coverage -fprofile-abs-path -fprofile-update=atomic
LDFLAGS += --coverage
endif
CFLAGS += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)
CFLAGS_STAMP := $(BUILD_DIR)/.test.cflags

SANITIZE_CC ?= clang
SANITIZE_BUILD_DIR := $(BUILD_DIR)/sanitize
SANITIZE_COMMON_FLAGS := -O1 -g -fno-omit-frame-pointer
SANITIZE_MEMORY_FLAGS := $(SANITIZE_COMMON_FLAGS) -fsanitize=memory -fsanitize-memory-track-origins=2
SANITIZE_UNDEFINED_FLAGS := $(SANITIZE_COMMON_FLAGS) -fsanitize=undefined -fno-sanitize-recover=undefined
SANITIZE_STACK_FLAGS := $(SANITIZE_COMMON_FLAGS) -fsanitize=safe-stack
SANITIZE_ADDRESS_FLAGS := $(SANITIZE_COMMON_FLAGS) -fsanitize=address
SANITIZE_LEAK_FLAGS := $(SANITIZE_COMMON_FLAGS) -fsanitize=leak
SANITIZE_THREAD_FLAGS := $(SANITIZE_COMMON_FLAGS) -fsanitize=thread
SANITIZE_INTEGER_FLAGS := $(SANITIZE_COMMON_FLAGS) -fsanitize=integer -fno-sanitize=implicit-conversion -fno-sanitize-recover=integer
SANITIZE_BOUNDS_FLAGS := $(SANITIZE_COMMON_FLAGS) -fsanitize=bounds -fno-sanitize-recover=bounds
SANITIZE_OBJECT_SIZE_FLAGS := $(SANITIZE_COMMON_FLAGS) -fsanitize=object-size -fno-sanitize-recover=object-size

PERF ?= perf
PERF_DATA ?= $(BUILD_DIR)/perf.data
PERF_CALL_GRAPH ?= dwarf
PERF_RECORD_FLAGS ?= -F 999 -g --call-graph $(PERF_CALL_GRAPH)
PERF_REPORT_FLAGS ?= --stdio --stdio --sort=symbol --percentage=absolute -g none

TEST_EXE := $(BUILD_DIR)/test
TEST_OBJ := $(BUILD_DIR)/test.o
PET_SPEC_OBJ := $(BUILD_DIR)/nvmeib_pet_specification.o
TEST_OBJS := $(TEST_OBJ) $(PET_SPEC_OBJ)
TEST_DCT := $(BUILD_DIR)/dict.${COMMIT_ID}.json
TEST_OUT := $(BUILD_DIR)/messages.pet

LCOV ?= lcov
GENHTML ?= genhtml
COVERAGE_DIR := $(BUILD_DIR)/coverage
COVERAGE_RAW_LCOV := $(COVERAGE_DIR)/coverage.raw.lcov
COVERAGE_LCOV := $(COVERAGE_DIR)/coverage.lcov

.PHONY: all clean test check dirs demo demo_dict perf coverage sanitize sanitize_memory sanitize_undefined sanitize_stack sanitize_address sanitize_leak sanitize_thread sanitize_integer sanitize_bounds sanitize_object_size ci fast_build FORCE

all: dirs $(TEST_EXE)

dirs:
	@mkdir -p $(BUILD_DIR) $(ROTATIONS_DIR)

$(CFLAGS_STAMP): FORCE | dirs
	@printf '%s\n' '$(CFLAGS)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm $@.tmp; fi

$(BUILD_DIR)/test.o: $(TESTS_DIR)/test.c nvmeib_pet_specification.h nvmeib_pet_types.h $(CFLAGS_STAMP) | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/nvmeib_pet_specification.o: nvmeib_pet_specification.c nvmeib_pet_specification.h nvmeib_pet_types.h $(CFLAGS_STAMP) | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_EXE): $(TEST_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

test: dirs $(TEST_EXE)
	rm -f $(ROTATIONS_DIR)/seed_*.pet \
		$(ROTATIONS_DIR)/seed_*.txt
	./$(TEST_EXE) $(TEST_OUT) $(ROTATIONS_DIR)

check: test

demo_dict: dirs test
	PY=$(PY) ./pet_messages.sh save-dictionary $(TEST_EXE) $(TEST_DCT)

demo: dirs test demo_dict
	PY=$(PY) ./pet_messages.sh view --no-sort $(BUILD_DIR) $(TEST_OUT)
	@echo "Comparing generated rotation journals with viewer output..."
	@for pet in $(ROTATIONS_DIR)/seed_*.pet; do \
		test -e "$$pet" || { echo "No rotation PET files found in $(ROTATIONS_DIR)"; exit 1; }; \
		expected="$${pet%.pet}.expected.txt"; \
		viewer="$${pet%.pet}.viewer.txt"; \
		PY=$(PY) ./pet_messages.sh view-raw "$$pet" > "$$viewer"; \
		test -s "$$expected"; \
		test -s "$$viewer"; \
		diff -u "$$expected" "$$viewer"; \
	done
	@echo "Rotation viewer outputs generated and compared: $(ROTATIONS_DIR)"

perf: $(TEST_EXE)
	$(PERF) record $(PERF_RECORD_FLAGS) -o $(PERF_DATA) -- $(TEST_EXE)
	$(PERF) report $(PERF_REPORT_FLAGS) -i $(PERF_DATA) 2>&1 | rg -o "^\s+[0-9].*pet[^\s]*" | head -40

coverage:
	@command -v $(LCOV) >/dev/null || { echo "$(LCOV) is required for coverage reports"; exit 1; }
	@command -v $(GENHTML) >/dev/null || { echo "$(GENHTML) is required for coverage reports"; exit 1; }
	$(MAKE) clean
	$(MAKE) test COVERAGE=1
	rm -rf $(COVERAGE_DIR)
	mkdir -p $(COVERAGE_DIR)
	$(LCOV) --capture \
		--directory $(BUILD_DIR) \
		--output-file $(COVERAGE_RAW_LCOV) \
		--rc branch_coverage=1 \
		--filter range \
		--ignore-errors mismatch \
		--ignore-errors inconsistent
	@test -s $(COVERAGE_RAW_LCOV) || { echo "Coverage capture did not create $(COVERAGE_RAW_LCOV)"; exit 1; }
	$(LCOV) --remove $(COVERAGE_RAW_LCOV) \
		'*/tests/*' \
		'/usr/*' \
		--output-file $(COVERAGE_LCOV) \
		--rc branch_coverage=1 \
		--filter range \
		--ignore-errors inconsistent \
		--ignore-errors unused
	$(GENHTML) $(COVERAGE_LCOV) \
		--output-directory $(COVERAGE_DIR) \
		--branch-coverage \
		--title "PET Coverage" \
		--ignore-errors inconsistent \
		--ignore-errors corrupt \
		--ignore-errors unmapped
	@echo "Coverage report: $(COVERAGE_DIR)/index.html"
	@echo "LCOV tracefile: $(COVERAGE_LCOV)"

sanitize: sanitize_memory sanitize_undefined sanitize_stack sanitize_address sanitize_leak sanitize_thread sanitize_integer sanitize_bounds sanitize_object_size

sanitize_memory:
	@command -v $(SANITIZE_CC) >/dev/null || { echo "$(SANITIZE_CC) is required for sanitizer builds"; exit 1; }
	$(MAKE) test CC=$(SANITIZE_CC) BUILD_DIR=$(SANITIZE_BUILD_DIR)/memory EXTRA_CFLAGS="$(SANITIZE_MEMORY_FLAGS)" EXTRA_LDFLAGS="$(SANITIZE_MEMORY_FLAGS)"

sanitize_undefined:
	@command -v $(SANITIZE_CC) >/dev/null || { echo "$(SANITIZE_CC) is required for sanitizer builds"; exit 1; }
	$(MAKE) test CC=$(SANITIZE_CC) BUILD_DIR=$(SANITIZE_BUILD_DIR)/undefined EXTRA_CFLAGS="$(SANITIZE_UNDEFINED_FLAGS)" EXTRA_LDFLAGS="$(SANITIZE_UNDEFINED_FLAGS)"

sanitize_stack:
	@command -v $(SANITIZE_CC) >/dev/null || { echo "$(SANITIZE_CC) is required for sanitizer builds"; exit 1; }
	$(MAKE) test CC=$(SANITIZE_CC) BUILD_DIR=$(SANITIZE_BUILD_DIR)/stack EXTRA_CFLAGS="$(SANITIZE_STACK_FLAGS)" EXTRA_LDFLAGS="$(SANITIZE_STACK_FLAGS)"

sanitize_address:
	@command -v $(SANITIZE_CC) >/dev/null || { echo "$(SANITIZE_CC) is required for sanitizer builds"; exit 1; }
	$(MAKE) test CC=$(SANITIZE_CC) BUILD_DIR=$(SANITIZE_BUILD_DIR)/address EXTRA_CFLAGS="$(SANITIZE_ADDRESS_FLAGS)" EXTRA_LDFLAGS="$(SANITIZE_ADDRESS_FLAGS)"

sanitize_leak:
	@command -v $(SANITIZE_CC) >/dev/null || { echo "$(SANITIZE_CC) is required for sanitizer builds"; exit 1; }
	$(MAKE) test CC=$(SANITIZE_CC) BUILD_DIR=$(SANITIZE_BUILD_DIR)/leak EXTRA_CFLAGS="$(SANITIZE_LEAK_FLAGS)" EXTRA_LDFLAGS="$(SANITIZE_LEAK_FLAGS)"

sanitize_thread:
	@command -v $(SANITIZE_CC) >/dev/null || { echo "$(SANITIZE_CC) is required for sanitizer builds"; exit 1; }
	$(MAKE) test CC=$(SANITIZE_CC) BUILD_DIR=$(SANITIZE_BUILD_DIR)/thread EXTRA_CFLAGS="$(SANITIZE_THREAD_FLAGS)" EXTRA_LDFLAGS="$(SANITIZE_THREAD_FLAGS)"

sanitize_integer:
	@command -v $(SANITIZE_CC) >/dev/null || { echo "$(SANITIZE_CC) is required for sanitizer builds"; exit 1; }
	$(MAKE) test CC=$(SANITIZE_CC) BUILD_DIR=$(SANITIZE_BUILD_DIR)/integer EXTRA_CFLAGS="$(SANITIZE_INTEGER_FLAGS)" EXTRA_LDFLAGS="$(SANITIZE_INTEGER_FLAGS)"

sanitize_bounds:
	@command -v $(SANITIZE_CC) >/dev/null || { echo "$(SANITIZE_CC) is required for sanitizer builds"; exit 1; }
	$(MAKE) test CC=$(SANITIZE_CC) BUILD_DIR=$(SANITIZE_BUILD_DIR)/bounds EXTRA_CFLAGS="$(SANITIZE_BOUNDS_FLAGS)" EXTRA_LDFLAGS="$(SANITIZE_BOUNDS_FLAGS)"

sanitize_object_size:
	@command -v $(SANITIZE_CC) >/dev/null || { echo "$(SANITIZE_CC) is required for sanitizer builds"; exit 1; }
	$(MAKE) test CC=$(SANITIZE_CC) BUILD_DIR=$(SANITIZE_BUILD_DIR)/object_size EXTRA_CFLAGS="$(SANITIZE_OBJECT_SIZE_FLAGS)" EXTRA_LDFLAGS="$(SANITIZE_OBJECT_SIZE_FLAGS)"

ci:
	$(MAKE) demo
	$(MAKE) sanitize
	$(MAKE) perf

clean:
	rm -rf $(BUILD_DIR)

fast_build: dirs $(TEST_EXE) test
