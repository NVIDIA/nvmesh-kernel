#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# View IO-PET traces produced by blk_unitest.
# Wraps common/pet/pet_messages.sh view (which runs under poetry).
#
# Usage: view_pet_messages.sh [--release] [--sort] <trace-file> [<trace-file> ...]

set -euo pipefail

THIS_DIR="$(dirname "$(realpath "$0")")"
PET_MESSAGES_SH="$THIS_DIR/../../../common/pet/pet_messages.sh"

BUILD_TYPE=debug
SORT_FLAG=--no-sort

args=()
for arg in "$@"; do
    case "$arg" in
        --release) BUILD_TYPE=release ;;
        --sort)    SORT_FLAG= ;;
        *)         args+=("$arg") ;;
    esac
done

DICTS_DIR="$THIS_DIR/99bin/$BUILD_TYPE/io_pet"

exec "$PET_MESSAGES_SH" view ${SORT_FLAG:+"$SORT_FLAG"} "$DICTS_DIR" "${args[@]}"
