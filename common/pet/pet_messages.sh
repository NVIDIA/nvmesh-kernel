#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Wrapper for nvmeib_pet_messages.py that manages a Python virtual environment with the
# required PET dependencies. All arguments are passed through to the Python script.
#
# The venv is created on first use and reused on subsequent calls. Dependencies are
# installed from the internal NVIDIA Artifactory PyPI mirror using pinned versions
# from requirements.txt.
#
# Environment variables:
#   PET_VENV         - Virtual environment directory (default: <script_dir>/.venv)
#   NVIDIA_PYPI_URL  - PyPI index URL (default: nv-shared-pypi on urm.nvidia.com)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

NVIDIA_PYPI_URL="${NVIDIA_PYPI_URL:-https://urm.nvidia.com/artifactory/api/pypi/nv-shared-pypi/simple}"
PET_VENV="${PET_VENV:-$SCRIPT_DIR/.venv}"

if [ ! -f "$PET_VENV/bin/python3" ]; then
    echo "Creating PET virtual environment at $PET_VENV..."
    python3 -m venv --without-pip --system-site-packages "$PET_VENV"
    "$PET_VENV/bin/python3" -m pip install --ignore-installed \
        --index-url "$NVIDIA_PYPI_URL" -r "$SCRIPT_DIR/requirements.txt"
fi

exec "$PET_VENV/bin/python3" "$SCRIPT_DIR/nvmeib_pet_messages.py" "$@"
