CC := gcc
CFLAGS := -Wall -Wextra -O2 -fPIC
BUILD_DIR := build
SRC := nvmeib_jdr.c
OBJ := $(BUILD_DIR)/nvmeib_jdr.o

LIBNAME := libnvmeib_jdr
STATIC_LIB := $(BUILD_DIR)/$(LIBNAME).a
SHARED_LIB := $(BUILD_DIR)/$(LIBNAME).so

MODE ?= both

all: $(MODE)

both: static shared

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

static: $(BUILD_DIR) $(OBJ)
	ar rcs $(STATIC_LIB) $(OBJ)
	@echo "Built static library: $(STATIC_LIB)"

shared: $(BUILD_DIR) $(OBJ)
	$(CC) -shared -o $(SHARED_LIB) $(OBJ)
	@echo "Built shared library: $(SHARED_LIB)"

$(OBJ): $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

test:
	$(MAKE) -C test

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build artifacts"

.PHONY: all static shared both test clean

