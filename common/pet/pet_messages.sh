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

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
if [[ -f "$SCRIPT_DIR/pypi_sources.sh" ]]; then
    source "$SCRIPT_DIR/pypi_sources.sh"
else
    source "$SCRIPT_DIR/../../pypi_sources.sh"
fi

PET_VENV="${PET_VENV:-$SCRIPT_DIR/.venv-py${PY:-3}}"

if ! command -v "python${PY:-3}" >/dev/null 2>&1; then
    echo "${BASH_SOURCE[0]}: python${PY:-3} not found" >&2
    echo "PY to override the default python version: e.g., PY=3.10 ${BASH_SOURCE[0]}" >&2
    exit 1
fi

if ! "python${PY:-3}" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)'; then
    echo "${BASH_SOURCE[0]}: Python 3.9 or newer required; got $(python${PY:-3} --version 2>&1)" >&2
    echo "PY to override the default python version: e.g., PY=3.10 ${BASH_SOURCE[0]}" >&2
    exit 1
fi

if [[ ! -f "$PET_VENV/bin/python${PY:-3}" ]]; then
    PYPI_URL=$(get_pypi_url)
    echo "Creating PET virtual environment at $PET_VENV with PyPI index $PYPI_URL..."
    python${PY:-3} -m venv --without-pip --system-site-packages "$PET_VENV"
    "$PET_VENV/bin/python${PY:-3}" -m pip install --ignore-installed \
        --index-url "$PYPI_URL" -r "$SCRIPT_DIR/requirements.txt"
fi

exec "$PET_VENV/bin/python${PY:-3}" "$SCRIPT_DIR/nvmeib_pet_messages.py" "$@"
