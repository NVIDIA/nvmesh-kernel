#!/bin/bash

pidPath="/var/run/nvmesh"

cd "$(dirname "$0")"

if [ ! -d /sys/module/ib_uverbs ]; then
	echo "Loading module ib_uverbs"
	sudo modprobe ib_uverbs
fi

DIR=.

if (( $# >= 1 )); then
	if [[ "$1" == "debug" ]]; then
		DIR=bin/debug
		shift
	elif [[ "$1" == "release" ]]; then
		DIR=bin/release
		shift
	elif [[ "$1" == "delease" ]]; then
		DIR=bin/delease
		shift
	fi
fi

if [ -d $pidPath ]; then
	mkdir $pidPath
fi

# (sudo bash -c "ulimit -c unlimited; stdbuf -i0 -o0 -e0 ./nvmeibt_toma") 2> log_error > log_std
sudo bash -c "ulimit -c unlimited; nice -n -15 /usr/bin/stdbuf -i0 -o0 -e0 $DIR/nvmeibt_toma $*"
# sudo bash -c "ulimit -c unlimited; nice -n -15 /usr/bin/stdbuf -i0 -o0 -e0 valgrind --suppressions=./nvmeibt_valgrind_suppress.conf --max-stackframe=2483352 $DIR/nvmeibt_toma $*"
