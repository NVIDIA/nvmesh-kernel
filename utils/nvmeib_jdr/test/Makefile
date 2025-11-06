CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -I../
BUILD_DIR = ../build
LIB_STATIC = $(BUILD_DIR)/libnvmeib_jdr.a
LDFLAGS  = $(LIB_STATIC)
TESTS    = simple

all: $(TESTS)

$(TESTS): %: %.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: all
	@echo "Running tests..."
	@for t in $(TESTS); do echo "---- $$t ----"; ./$$t || exit 1; done

clean:
	rm -f $(TESTS)

.PHONY: all clean run

