#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

NVIDIA_PYPI_URL="${NVIDIA_PYPI_URL:-https://urm.nvidia.com/artifactory/api/pypi/nv-shared-pypi/simple}"
PYPI_URL="${PYPI_URL:-https://pypi.org/simple}"

function is_nvidia_pypi_reachable() {
    command -v curl >/dev/null 2>&1 && curl -s --head --fail --connect-timeout 5 --max-time 10 "$NVIDIA_PYPI_URL" > /dev/null
    return $?
}

function get_pypi_url() {
    if ! is_nvidia_pypi_reachable; then
        echo "Warning: NVIDIA PyPI URL $NVIDIA_PYPI_URL is not reachable by curl, fallback to ${PYPI_URL}" >&2
        echo "${PYPI_URL}"
    else
        echo "${NVIDIA_PYPI_URL}"
    fi
    return 0
}

function is_poetry_source_present() {
    local nv_shared_source_name=$1
    if command -v poetry >/dev/null 2>&1 && poetry source show $nv_shared_source_name| grep -q $nv_shared_source_name &>/dev/null; then
        return 0
    else
        return 1
    fi
}

# Remove NVIDIA PyPI source from Poetry if it is not reachable, fallback to pypi.org,
# and regenerate poetry.lock for the slow path.
function poetry_remove_nvidia_source_if_unreachable() {
    local nv_shared_source_name="nv-shared"
    if ! is_nvidia_pypi_reachable && is_poetry_source_present $nv_shared_source_name; then
        echo "Removing NVIDIA PyPI source from Poetry"
        poetry source remove $nv_shared_source_name
        echo "Regenerating poetry.lock for the default ${PYPI_URL} path..."
        poetry lock --regenerate
    fi
    return 0
}
