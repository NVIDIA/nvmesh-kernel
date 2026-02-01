BUILD_DIR := build
TESTS_DIR := tests

#the developer is expected to install it
KSC = kaitai-struct-compiler

# Input and output files
PET_SPEC_FILE = nvmeib_pet_specification.ksy
PET_BINFILE_READER = nvmeib_pet_archive.py

CC := gcc
CFLAGS := -Wall -Wextra -ggdb -O2 -I"." -I".."

TEST_EXE := $(BUILD_DIR)/test
TEST_OBJ := $(BUILD_DIR)/test.o
TEST_DCT := $(BUILD_DIR)/messages.dict
TEST_OUT := $(BUILD_DIR)/messages.pet

.PHONY: all clean test dirs demo

# Default target: build PDFs
all: $(PET_BINFILE_READER) dirs $(TEST_EXE) test demo

$(PET_BINFILE_READER): $(PET_SPEC_FILE)
	$(KSC) --target python $(PET_SPEC_FILE)

dirs:
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test.o: $(TESTS_DIR)/test.c nvmeib_pet_specification.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_EXE): $(TEST_OBJ)
	$(CC) $^ -o $@

test: dirs $(TEST_EXE)
	./$(TEST_EXE) $(TEST_OUT)

demo_dict: dirs test $(PET_BINFILE_READER)
	./nvmeib_pet_messages.py save-dictionary $(TEST_EXE) test_pet_msgs $(TEST_DCT)


demo: dirs test demo_dict $(PET_BINFILE_READER)
	./nvmeib_pet_messages.py view $(TEST_DCT) $(TEST_OUT)

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(PET_BINFILE_READER)



