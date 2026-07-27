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
	local root_dir=.
	local venv_dir="${root_dir}/.venv"

	cd "${root_dir}"

	# Detect virtual command to use
	if python2 -m virtualenv --help &>/dev/null; then
		virtualenvcmd="python2 -m virtualenv"
	elif type virtualenv &>/dev/null; then
		virtualenvcmd="virtualenv"
	else
		echo "Could not detect virtualenv command"
		return 1
	fi

	if [ ! -f "${venv_dir}/bin/activate" ]; then # If no python venv exists - create
		${virtualenvcmd} "${venv_dir}"
		. "${venv_dir}/bin/activate" # Activate
		pip install --upgrade pip >/dev/null
		pip install -r "${root_dir}/requirements.txt" >/dev/null # Update
	fi

}

__main
