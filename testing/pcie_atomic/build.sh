#!/bin/bash

# do not sync anything mentioned in $EXCLUDES

# hosts
REMOTE_SERVERS="nvme62: nvme68: nvme46:"
REMOTE_DIR="src/ssda/testing/pcie_atomic"

OPTS="--force --ignore-errors --compress --cvs-exclude --include=core --delete -av"

for rs in $REMOTE_SERVERS; do
	machine_name=`echo $rs | cut -d: -f1`
	make_flags=`echo $rs | cut -d: -f2-`
	make_flags=${make_flags//:/ } # replace all : with spaces
	echo "now working on " $machine_name
	ssh $machine_name "mkdir -pv $REMOTE_DIR"
	rsync $OPTS . -e 'ssh' $machine_name:$REMOTE_DIR
	ssh $machine_name "cd $REMOTE_DIR; make $1 ${make_flags} all $machine_type"
done


