#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# Script to build and run nvmeib_hash tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "========================================"
echo "Building nvmeib_hash_test..."
echo "========================================"
make clean
make

echo ""
echo "========================================"
echo "Running tests..."
echo "========================================"
./nvmeib_hash_test

exit $?








