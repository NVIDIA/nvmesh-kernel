#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

perror() {
    local _err=$?
    >&2 echo "$@ (${_err})"
    return ${_err}
}

die() {
    perror "$@"
    exit $?
}

prompt-yn() {
    read -p "${1} (Y/N) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        return 0
    else
        return 1
    fi
}

activate_venv() {
    if [ ! -d "${ROOTDIR}" ]; then
        prompt-yn "Infrastructure project root not found at '${ROOTDIR}', clone?" || { perror "Aborted"; return 126; }
        git clone "${INFRA_REMOTE}" "${ROOTDIR}" || { perror "Error: Cloning project"; return $?; }
    fi
    if [ ! -d "${MGMTDIR}" ]; then
        prompt-yn "Management project root not found at '${MGMTDIR}', clone?" || { perror "Aborted"; return 126; }
        git clone "${MGMT_REMOTE}" "${MGMTDIR}" || { perror "Error: Cloning project"; return $?; }
    fi
    if [ ! "$(command -v virtualenv)" ]; then
        prompt-yn "Command 'virtualenv' not found, install?" || { perror "Aborted"; return 126; }
        sudo dnf install virtualenv || { perror "Error: Isntalling virtualenv"; return $?; }
    fi
    if [ ! -f "${VENV_ACTIVATE}" ]; then
        prompt-yn "No virtual env found at '${VENV}', create?" || { perror "Aborted"; return 126; }
        virtualenv -p /usr/bin/python2 "${VENV}" || { perror "Error: Creating venv"; return $?; }
        source "${VENV_ACTIVATE}" || { perror "Error: Sourcing venv"; return $?; }
        pip install -r "${VENV_REQ}" || { perrror "Error: Installing requirements"; return $?; }
    else
        source "${VENV_ACTIVATE}" || { perror "Error: Sourcing venv"; return $?; }
    fi
    return 0
}

################################# Main #################################

# Read optional args
ROOTDIR=~/projects/infrastructure
MGMTDIR=~/projects/management
if [ "$1" == '--rootdir' ]; then
    ROOTDIR="$2"
    shift
    shift
fi

# Init variables
INFRA_REMOTE="git@gitlab.excelero.com:infrastructure/infrastructure.git"
MGMT_REMOTE="git@gitlab.excelero.com:management/management.git"
VENV_NAME="venv"
VENV="${ROOTDIR}/${VENV_NAME}"
VENV_ACTIVATE="${VENV}/bin/activate"
VENV_REQ="$( dirname "${BASH_SOURCE[0]}" )/connect_interactive.requirements.txt"
CONNECT_PY="$( dirname "${BASH_SOURCE[0]}" )/connect_interactive.py"

# Activate venv
activate_venv || exit $?

# Work
export PYTHONPATH="${ROOTDIR}:${MGMTDIR}:${PYTHONPATH}"
python "${CONNECT_PY}" "$@"
