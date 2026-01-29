#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set -e

SPEC_PATH=${SPEC_PATH:-$(dirname $(readlink -e $0))}
# TODO: no need for infra dir under mgmt dir since we removed the SDK dependency.
# One day we will need to change this. Today is not that day.
export PATH=$PATH:~/.local/bin
type poetry || curl -sSL https://install.python-poetry.org | python${PY:-3} -
set -x
[ -n "$PY" ] && poetry env use "$PY"
poetry install --sync --with=compile --no-root
poetry run pyinstaller $SPEC_PATH/tools.spec --log-level WARN --clean --workpath=$(mktemp -d) -y
ls -l dist/*
