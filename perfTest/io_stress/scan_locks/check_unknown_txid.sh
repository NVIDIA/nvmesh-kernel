#!/bin/bash
THIS_FILE_PATH=$0;
THIS_FILE_NAME="./$(basename -- ${THIS_FILE_PATH})";

function check_unknown_txid_in_node() {
	c=$1
	n=$2
	#echo "Params: ${c}, ${n}";
	for x in /proc/nvmeibc/volumes/v"$c"ost"$n"??/status ; do
		for segment in `grep 'a=. p=. acm=' $x | sed 's/ \+/,/g'` ; do
			role=$(echo $segment | cut -d, -f2)
			disk=$(echo $segment | cut -d, -f4)
			lba1=$(echo $segment | cut -d, -f5)
			lbaN=$(echo $segment | cut -d, -f6)
			node=$(echo $segment | cut -d, -f7)

			[[ $node == 'Unknown' ]] && continue

			echo Checking $c $n $(echo $x | cut -d/ -f5) $role $disk $lba1 $lbaN $node
			echo ==========================================================
			ssh $node touch /tmp/check_unknown_txids
			unknown_txids=$(ssh $node flock -x /tmp/check_unknown_txids /opt/nvmesh/common-repo/tools/scan_locks_ec -filter=1{"$disk"=1:$((0x$lba1/32))-$(((0x$lbaN+1)/32))}* -conf_d 8 -conf_p 2 -conf_role $role | grep 'scan done' | cut -d= -f8 | cut -d'}' -f1)
			[[ $unknown_txids == '00000000000000000000' ]] && continue
			utxids=$(echo $unknown_txids | sed 's/^0\+//')
			echo Cluster=$c Volume=$(echo $x | cut -d/ -f5) Role=$role Disk=$disk Node=$node has $utxids unknown txids
			echo
			echo
		done
	done
	########################################
	# /opt/nvmesh/common-repo/tools/scan_locks_ec -filter=1{BTAY251201ZM7P6CGN.1=1:276104-5235208}* -conf_d 8 -conf_p 2 -conf_role 1
	# scan done. {disk=BTAY251201ZM7P6CGN.1      , stales=00000000000000000000, stalessp=00000000000000000000, taken=00000000000000000000, dbits=00000000000000000000, unk{db=00000000000000000000, txid=00000000000001445924}, resets=00000000000000000000, mem_corrupt=00000000000000000000}
}

function check_unknown_txid_in_cluster() {
	c=$1
	declare -A n
	for x in {1..10}; do
		n[$x]=$x
	done
	for x in $@ ; do
		n[$(echo $x | cut -d= -f1)]=$(echo $x | cut -d= -f2)
	done
	for x in {1..10}; do
		node=in-c"$c"-n${n[$x]}
		echo "Processing node: ${node}";
		scp ${THIS_FILE_NAME} "$node":
			ssh $node ${THIS_FILE_NAME} node $c $x >& out.$x &
	done
	wait
	for x in {1..10}; do
		cat out.$x
	done
}

function fs01_all() {
	for x in {1001..1006}; do
		scp ${THIS_FILE_PATH} in-c"$x"-n1:
	done

	# ssh in-c1001-n1 .${THIS_FILE_NAME} cluster 1001 5=6 >& out.1001 &
	for x in {1001..1006}; do
		ssh in-c"$x"-n1 ${THIS_FILE_NAME} cluster $x  >& out.$x &
	done
	wait
	/bin/rm out.fs01
	for x in {1001..1006}; do
		cat out.$x >> out.fs01
	done
}

function fs02_all() {
	for x in {1008..1013}; do
		scp ${THIS_FILE_PATH} in-c"$x"-n1:
	done
	ssh in-c1008-n1 ${THIS_FILE_NAME} cluster 1008 8=7 >& out.1008 &
	for x in {1009..1013}; do
		ssh in-c"$x"-n1 .${THIS_FILE_NAME} cluster $x  >& out.$x &
	done
	wait
	/bin/rm out.fs02
	for x in {1008..1013}; do
		cat out.$x >> out.fs02
	done
}

# ---------------------------------------------------------------------
case "$1" in
  node*)	check_unknown_txid_in_node    ${@:2};;
  clus*)	check_unknown_txid_in_cluster ${@:2};;
  fs01*)	fs01_all ${@:2};;
  fs02*)	fs02_all ${@:2};;
  *)
	echo -e $"  Usage: ${THIS_FILE_NAME} [cmd], to clean unknown txids";
	echo -e "    \tnode    - Clean in single node";
	echo -e "    \tcluster - Clean in generic cluster";
	echo -e "    \tfs01    - Clean in cluster fs01";
	echo -e "    \tfs02    - Clean in cluster fs02";
	exit 2
esac
RETVAL=$?
echo
exit $RETVAL
