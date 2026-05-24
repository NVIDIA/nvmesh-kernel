# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

CC      ?= gcc
CFLAGS  ?= -O2 -g -ggdb -Wall -Wextra -Wno-unused-parameter \
           -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64

BIN     := siw_4k_mr_blk_test
SRC     := siw_4k_mr_blk_test.c

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BIN) *.o

.PHONY: all clean
