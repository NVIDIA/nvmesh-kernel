#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# run.sh — run two setup configurations, benchmark each, then compare results.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SETUP="${SCRIPT_DIR}/setup.sh"
WORKLOAD="${SCRIPT_DIR}/workload.sh"
COMPARE="${SCRIPT_DIR}/compare_fio_perf_runs.py"

OUTPUT_DIR="${1:-/tmp/raid1_perf_$(date +%Y%m%d_%H%M%S)}"

BASELINE_OPTS="options nvmeibc lock_ch_get_method=0 lock_shard_type=0 disk_locks_use_max_rd_atomic_limit=N nvmeibc_io_pet_disable=0"
CANDIDATE_OPTS="options nvmeibc lock_ch_get_method=0 lock_shard_type=0 disk_locks_use_max_rd_atomic_limit=Y nvmeibc_io_pet_disable=1"

BASELINE_DIR="${OUTPUT_DIR}/baseline"
CANDIDATE_DIR="${OUTPUT_DIR}/candidate"

echo "======================================================================"
echo "Output root: ${OUTPUT_DIR}"
echo "======================================================================"

echo ""
echo "=== [1/4] Setup: baseline ==="
bash "${SETUP}" "${BASELINE_OPTS}"

echo ""
echo "=== [2/4] Workload: baseline ==="
bash "${WORKLOAD}" "${BASELINE_DIR}" --size 3G --io-size 6G

echo ""
echo "=== [3/4] Setup: candidate ==="
bash "${SETUP}" "${CANDIDATE_OPTS}"

echo ""
echo "=== [4/4] Workload: candidate ==="
bash "${WORKLOAD}" "${CANDIDATE_DIR}" --size 3G --io-size 6G

echo ""
echo "=== [5/5] Compare ==="
python3 "${COMPARE}" "${BASELINE_DIR}" "${CANDIDATE_DIR}" \
    --output "${OUTPUT_DIR}/report.md"

echo ""
echo "Report written to: ${OUTPUT_DIR}/report.md"
