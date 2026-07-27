#!/bin/bash

# Fail on the first error
set -e

function __main() {
	local root_dir=.
	local venv_dir="${root_dir}/.venv"
	local wipe=no
	local virtualenvcmd=
	
	# Detect virtual command to use
	if python2 -m virtualenv --help &>/dev/null; then
		virtualenvcmd="python2 -m virtualenv"
	elif type virtualenv &>/dev/null; then
		virtualenvcmd="virtualenv"
	else
		echo "Could not detect virtualenv command"
		return 1
	fi

	while [[ $# -gt 0 ]]; do
		arg="$1"
		case $arg in
		"--wipe")
			shift
			wipe=yes
			;;
		"--venv")
			venv_dir=$2
			shift 2
			;;
		*)
			break
			;;
		esac
	done

	cd "${root_dir}"

	if [ "${wipe}" == "yes" ]; then
		echo "Wiping ${venv_dir}"
		rm -rf "${venv_dir}"
	fi

	if [ ! -f "${venv_dir}/bin/activate" ]; then # If no python venv exists - create
		echo "Virtual env not found, creating"
		${virtualenvcmd} "${venv_dir}"
		. "${venv_dir}/bin/activate" # Activate
		pip2 install -r "${root_dir}/requirements.txt" >/dev/null # Update
	else
		. "${venv_dir}/bin/activate" # Activate
	fi

	python ${root_dir}/corecomm_controller.py "$@"

}

__main "$@"
