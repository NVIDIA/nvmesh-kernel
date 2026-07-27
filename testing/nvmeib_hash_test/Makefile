# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu99 -g
LDFLAGS = -pthread

TARGET = nvmeib_hash_test

$(TARGET): nvmeib_hash_test.c
	$(CC) $(CFLAGS) -o $(TARGET) nvmeib_hash_test.c $(LDFLAGS)

.PHONY: clean test

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)






