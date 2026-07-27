#!/bin/bash

cd /tmp/nvmesh

source ./transfer_lists

if [ -z "$1" ]; then
	exit 1
fi

if [ -d "$1/bin_target" ]; then
	rm -rf $1/bin_target
fi
cp -r --parents $target_scripts_transfer_list $1
mv $1/bin $1/bin_target

if [ -d "$1/bin_client" ]; then
        rm -rf $1/bin_client
fi
cp -r --parents $client_scripts_transfer_list $1
mv $1/bin $1/bin_client

cp -r --parents $common_scripts_transfer_list $1

cd -
