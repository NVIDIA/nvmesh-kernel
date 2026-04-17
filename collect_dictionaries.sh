#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

source pypi_sources.sh
set -e #exit on first error
echo "Collecting dictionaries for binary tracing..."

# Commit id: from environment, or from version file, or "unknown"
COMMIT_ID="${COMMIT_ID:-}"
if [ -z "$COMMIT_ID" ] && [ -f version ]; then
	COMMIT_ID=$(sed -n 's/^commit="\(.*\)"$/\1/p' version)
fi
COMMIT_ID="${COMMIT_ID:-unknown}"

TMPDIR=$(mktemp -d)
trap "rm -rf ${TMPDIR}" EXIT
PET_DIR="${TMPDIR}/pet_dictionaries"
mkdir -p "${PET_DIR}"

# Collect trace dictionaries (dict.*.json, libfmtrs.so)
find . \
	-name 'dict.*.json' \
	-o -name 'libfmtrs.so' \
	| xargs -I{} cp -f {} "${TMPDIR}" 2>/dev/null || true

# Build PET dictionary from client module if present
# Section .nvmeibc_io_pet_msgs is where PET strings live in the client kernel module
PET_MODULE="${PET_MODULE:-}"
PET_SECTION="${PET_SECTION:-.nvmeibc_io_pet_msgs}"
if [ -z "$PET_MODULE" ]; then
	echo "PET_MODULE is not set, no PET dictionary will be built"
else
	echo "PET_MODULE is set to $PET_MODULE"
fi

# We may not always be able to install the python version we want or poetry or
# the dependencies. The infra team prefers to manage the lifecycle of Python,
# Poetry and dependencies separately and skip the PET dictionary build if
# poetry is not available, rather than fall back to installing a specific
# poetry version.
export PATH="${PATH}:${HOME}/.local/bin"
if ! command -v poetry >/dev/null 2>&1; then
	echo "poetry not found, skip PET dictionary build"
fi

if [ -n "$PET_MODULE" ] && [ -f "$PET_MODULE" ] && command -v poetry >/dev/null 2>&1; then
	poetry_remove_nvidia_source_if_unreachable
	PET_DICT="${PET_DIR}/dict.${COMMIT_ID#0x}.json"
	# In case there are multiple python versions installed, use the one specified by PY.
	if [[ -n "$PY" ]]; then
		if ! poetry env use "$PY"; then
			echo "Failed to use Python version $PY"
			exit 1
		fi
		echo "Poetry configured to use Python version $PY"
	fi
	# Verify required packages with poetry.
	start=$SECONDS
	# Take the poetry.lock file as the source of truth, regenerate if it is not up-to-date.
	if ! poetry lock --no-update; then
		echo "Warning: poetry.lock out of sync, regenerating with dependency updates..."
		poetry lock
	fi

	poetry install --no-root --only pet
	echo "poetry verifies required packages in $((SECONDS - start)) seconds"

	# Use the Python version specified by PY, or default to python3
	PET_PYTHON="poetry run python${PY:-3}"
	echo "poetry run python is set to $PET_PYTHON"
	start=$SECONDS
	if $PET_PYTHON common/pet/nvmeib_pet_messages.py save-dictionary "$PET_MODULE" "$PET_SECTION" "$PET_DICT"; then
		runtime=$((SECONDS - start))
		echo "PET dictionary saved to ${PET_DICT} in ${runtime} seconds"
	else
		rm -f "$PET_DICT"
		echo "Warning: could not build PET dictionary from ${PET_MODULE}"
		exit 1
	fi
fi

# EPOCH used by compilator to have determinstic timestamps in tar.
tar "${SOURCE_DATE_EPOCH:+--mtime=$(date -d "@$SOURCE_DATE_EPOCH" '+%F %T')}" -cvzf dictionaries.tar.gz -C ${TMPDIR} $(cd ${TMPDIR} && ls)

echo "Collection dictionaries done (commit=${COMMIT_ID})"
