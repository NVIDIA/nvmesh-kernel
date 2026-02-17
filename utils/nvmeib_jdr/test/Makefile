CC          = gcc
BASE_CFLAGS = -Wall -Wextra -O2 -I../ -ggdb
CFLAGS      ?= $(BASE_CFLAGS)
LIB_BUILD_DIR = build-test
LIB_STATIC  = ../$(LIB_BUILD_DIR)/libnvmeib_jdr.a
LIB_TO_LINK ?= $(LIB_STATIC)
LDFLAGS     ?= $(LIB_TO_LINK)
TESTS       = simple

TEST_COV_CFLAGS = -Wall -Wextra -O0 -I../ -ggdb --coverage -fprofile-abs-path
LIB_COV_CFLAGS  = -Wall -Wextra -O0 -fPIC -ggdb --coverage -fprofile-abs-path
COV_LDFLAGS     = --coverage
COV_LIB_BUILD_DIR = build-cov
COV_LIB_STATIC  = ../$(COV_LIB_BUILD_DIR)/libnvmeib_jdr.a
COV_DIR         = coverage
COV_HTML        = $(COV_DIR)/index.html

all: $(TESTS)

$(LIB_STATIC):
	$(MAKE) -C .. static BUILD_DIR=$(LIB_BUILD_DIR)

$(COV_LIB_STATIC):
	$(MAKE) -C .. static BUILD_DIR=$(COV_LIB_BUILD_DIR) CFLAGS="$(LIB_COV_CFLAGS)"

$(TESTS): %: %.c $(LIB_TO_LINK)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: all
	@echo "Running tests..."
	@for t in $(TESTS); do echo "---- $$t ----"; ./$$t || exit 1; done

coverage:
	@python3 -m gcovr --version >/dev/null 2>&1 || { echo "python3 module 'gcovr' is required (pip install --user gcovr)"; exit 1; }
	$(MAKE) clean
	@echo "Rebuilding library with coverage flags..."
	$(MAKE) -C .. clean BUILD_DIR=$(COV_LIB_BUILD_DIR)
	$(MAKE) $(COV_LIB_STATIC)
	@echo "Building tests with coverage flags..."
	$(MAKE) all LIB_TO_LINK="$(COV_LIB_STATIC)" CFLAGS="$(TEST_COV_CFLAGS)" LDFLAGS="$(COV_LIB_STATIC) $(COV_LDFLAGS)"
	@echo "Running tests..."
	@for t in $(TESTS); do echo "---- $$t ----"; ./$$t || exit 1; done
	@echo "Generating HTML report..."
	rm -rf $(COV_DIR)
	mkdir -p $(COV_DIR)
	python3 -m gcovr -r .. --object-directory . --object-directory ../$(COV_LIB_BUILD_DIR) --html --html-details -o $(COV_HTML)
	@echo "Coverage report: $(COV_HTML)"

clean:
	rm -f $(TESTS)
	find . -type f \( -name '*.gcda' -o -name '*.gcno' -o -name '*.gcov' \) -delete
	rm -rf $(COV_DIR)

.PHONY: all clean run coverage
