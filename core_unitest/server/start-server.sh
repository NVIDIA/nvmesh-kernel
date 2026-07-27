#!/bin/bash

# Fail on first error
set -e

# Must run as root
if [[ ! $EUID -eq 0 ]]; then
    echo 'Must run as root'
    sudo "$0" "$@"
    exit $?
fi

function __main() {
    local pid
    local root_dir=.
    local venv_dir="${root_dir}/.venv"

    cd "${root_dir}"
    . "${venv_dir}/bin/activate" # Activate

    python2 ./corecomm_server.py "$@"
}

__main "$@"
