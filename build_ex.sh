#!/usr/local/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

BASH_EXE=/usr/local/bin/bash
$BASH_EXE ./build.sh "$@" | $BASH_EXE ./gcc_error_parser.sh
#$BASH_EXE ./build.sh
