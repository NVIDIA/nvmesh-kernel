# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

.PHONY: clean all
.DEFAULT_GOAL := all

crash_analyzer.so: /usr/include/crash/defs.h crash_analyzer.c
	gcc -Wall -Werror -I/usr/include -nostartfiles -shared -rdynamic -o crash_analyzer.so crash_analyzer.c -fPIC -DX86_64

all: crash_analyzer.so

install: all
	install -d /usr/lib64/crash/extensions/
	install crash_analyzer.so /usr/lib64/crash/extensions/

clean:
	@rm -f crash_analyzer.so
