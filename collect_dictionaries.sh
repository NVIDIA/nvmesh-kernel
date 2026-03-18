#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

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

if [ -n "$PET_MODULE" ] && [ -f "$PET_MODULE" ]; then
	PET_DICT="${PET_DIR}/dict.${COMMIT_ID#0x}.json"
	# Prefer poetry env (has pyelftools+pydantic from pyproject.toml) when available;
	# install poetry with the required packages for save-dictionary.
	PET_PYTHON="python3"
	if ! type poetry; then
		export PATH=$PATH:~/.local/bin
		curl -sSL https://install.python-poetry.org | ${PET_PYTHON} -
	fi
	poetry lock
	poetry install --sync --no-root --only pet
	PET_PYTHON="poetry run python3"
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
