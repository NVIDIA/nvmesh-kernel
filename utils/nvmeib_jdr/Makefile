CC := gcc
CFLAGS := -Wall -Wextra -O2 -fPIC -ggdb
BUILD_DIR := build

SRCS = nvmeib_txt.c nvmeib_jdr.c
OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)

LIBNAME := libnvmeib_jdr
STATIC_LIB := $(BUILD_DIR)/$(LIBNAME).a
SHARED_LIB := $(BUILD_DIR)/$(LIBNAME).so

MODE ?= both

all: $(MODE)

both: static shared

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

static: $(BUILD_DIR) $(OBJS)
	ar rcs $(STATIC_LIB) $(OBJS)
	@echo "Built static library: $(STATIC_LIB)"

shared: $(BUILD_DIR) $(OBJS)
	$(CC) -shared -o $(SHARED_LIB) $(OBJS)
	@echo "Built shared library: $(SHARED_LIB)"

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

test:
	$(MAKE) -C test

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build artifacts"

.PHONY: all static shared both test clean

