#!/bin/bash

# do not sync anything mentioned in $EXCLUDES
EXCLUDES=excludes

# hosts
# REMOTE_SERVERS="r9:IM_BOTH=yes r122:IM_BOTH=yes"
REMOTE_SERVERS="n45:IM_BOTH=yes n46:IM_BOTH=yes"
# REMOTE_SERVERS="r45:IM_BOTH=yes r46:IM_BOTH=yes"
REMOTE_DIR=projects/master/testing/rdma/kernel

OPTS="--force --ignore-errors --exclude-from=$EXCLUDES --delete -av"

for rs in $REMOTE_SERVERS; do
	machine_name=`echo $rs | cut -d: -f1`
	make_flags=`echo $rs | cut -d: -f2-`
	make_flags=${make_flags//:/ } # replace all : with spaces
	echo "now working on " $machine_name
	ssh $machine_name "mkdir -pv $REMOTE_DIR"
	rsync $OPTS . -e 'ssh' $machine_name:$REMOTE_DIR
	ssh $machine_name "cd $REMOTE_DIR; make $1 ${make_flags} all $machine_type"
done


