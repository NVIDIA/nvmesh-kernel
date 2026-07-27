#!/bin/sh
usage() {
	echo "$0 <dev:port> <dev:port> <runtime> where:"
	echo "    dev:port is the IB device name and port number e.g. mlx4_0:1"
	echo "    runtime is the time in seconds"
}

dev1_path="/sys/kernel/atom_test/dev1"
dev2_path="/sys/kernel/atom_test/dev2"
run_path="/sys/kernel/atom_test/run"

if [ $# != 3 ]; then
	usage
	exit -1
fi

if [ ! -f "$dev1_path" ]; then
	echo "$dev1_path does not exist. Is module loaded?"
	exit -1
fi

if [ ! -f "$dev2_path" ]; then
	echo "$dev2_path does not exist. Is module loaded?"
	exit -1
fi

if [ ! -f "$run_path" ]; then
	echo "$run_path does not exist. Is module loaded?"
	exit -1
fi

echo $1 > $dev1_path
if [ $? != 0 ]; then
	echo "Failed to set $1 as device 1. Check dmesg for details"
fi

echo $2 > $dev2_path
if [ $? != 0 ]; then
	echo "Failed to set $2 as device 2. Check dmesg for details"
fi

echo $3 > $run_path
if [ $? != 0 ]; then
	echo "Failed to set $3 as runtime or failed to start run. Check dmesg for details"
fi

running=`cat $run_path`
echo "$running"





