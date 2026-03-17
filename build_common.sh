#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# COMMIT_ID appears in `/proc/nvmeib[a|c]/version`. Without `--abbrev`, the value is of variable length, as
# Git uses the minimum number of hex characters needed to be unique. For smaller repositories, 4-7 characters can be sufficient.
# For larger repositories(e.g., Linux kernel), 8-12 characters are recommended. We configured the value with a fixed 12-character width.
function git_commit_id() {
    local abbrev="${1:-12}"

    if ! [[ "$abbrev" =~ ^[0-9]+$ ]]; then
        echo "Error: abbrev must be a number" >&2
        return 1
    fi

    if (( abbrev < 4 || abbrev > 40 )); then
        echo "Error: abbrev must be between 4 and 40" >&2
        return 1
    fi

    git log -n1 --format=%h --abbrev="$abbrev"
}

