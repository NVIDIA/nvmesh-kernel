#!/bin/sh

# Script for showing NVMesh Status
big_under="===================================================================================================="
small_under="-----------------------------------------------------------------------------------------------------"

up_format=""
down_format=""
head_format=""
subhead_format=""
under_format=""
text_format=""
reset_format=""

for param in "$@"; do
	if [ "$param" == "--color" ]; then
		up_format="\e[1m\e[32m"
		down_format="\e[1m\e[31m"
		head_format="\e[1m\e[93m"
		subhead_format="\e[1m\e[34m"
		under_format="\e[93m"
		text_format="\e[39m"
		reset_format="\e[0m"
	fi
done

proc_timeout="1" # In seconds


function module_status()
{
	# $1 - module name
	# $2 - module description
	echo -en "${subhead_format}${2}${reset_format}: "
	if [ -d /sys/module/$1 ]; then
		echo -en "${up_format}Up${reset_format}"
	else
		echo -en "${down_format}Down${reset_format}"
	fi
}


#Infiniband Status
echo -e "${head_format}Infiniband:\t${text_format}${reset_format}"
echo -e "${under_format}${big_under}${reset_format}"
module_status ib_core "IB Core"
echo -en "\t"
module_status ib_uverbs "IB UVerbs"
echo -en "\t"
module_status ib_cm "IB CM"
echo -en "\t"
module_status ib_ucm "IB UCM"
echo -en "\t"
module_status rdma_cm "RDMA CM"
echo -en "\t"
module_status rdma_ucm "RDMA UCM"
echo ""
module_status mlx4_core "MLX4 Core"
echo -en "\t"
module_status mlx4_ib "MLX4 IB"
echo -en "\t"
module_status mlx4_en "MLX4 EN"
echo -en "\t"
module_status mlx5_core "MLX5 Core"
echo -en "\t"
module_status mlx5_ib "MLX5 IB"
echo -en "\t"
module_status bnxt_re "BNXT RE"
echo -e "\n"

# NVMesh Config
source /etc/nvmesh/nvmesh.conf
for f in /etc/nvmesh/nvmesh.conf.d/*; do
   . $f
done

echo -e "${head_format}NVMesh Conf:\t${text_format}${reset_format}"
echo -e "${under_format}${big_under}${reset_format}"
echo -e "${subhead_format}Mgmt:\t${text_format}${MANAGEMENT_SERVERS}"
echo -e "${subhead_format}NICs:\t${text_format}${CONFIGURED_NICS}\n"
# Check if TOMA is up or down
export TOMA_PID=`pgrep toma`
if [[ ! -z $TOMA_PID ]]; then
	TOMA_BUILD_STATUS=`timeout $proc_timeout sudo grep "TOMA BUILD" -A1 /proc/nvmeibs/toma_status/raft | tail -1 | xargs | cut -c2- `
	TOMA_RAFT_STATUS=`timeout $proc_timeout sudo grep LEADER /proc/nvmeibs/toma_status/raft | xargs | cut -c2- `
	TOMA_CMD=`cat /proc/$TOMA_PID/cmdline | cut -d '' -f1`
	TOMA_PARAMS=`cat /proc/$TOMA_PID/cmdline | cut -d '' -f2- | tr '\0' ' '`
	echo -e "${up_format}TOMA is Up${reset_format}${text_format} (PID $TOMA_PID)${reset_format}\n${under_format}${big_under}${text_format}"
	echo -e "${subhead_format}Cmd:\t${text_format}${TOMA_CMD}"
	echo -e "${subhead_format}Params:\t${text_format}${TOMA_PARAMS}"
	echo -e "${subhead_format}Build:\t${text_format}${TOMA_BUILD_STATUS}"
	echo -e "${subhead_format}RAFT:\t${text_format}${TOMA_RAFT_STATUS}\n"
else
	TOMA_CRASH_FILE=`ls -rt /var/opt/nvmesh/core* 2>/dev/null | tail -1`
	if [[ ! -z $TOMA_CRASH_FILE ]]; then
		TOMA_CRASH_FILE_TIME=`date -r $TOMA_CRASH_FILE`
		TOMA_CRASH_BIN=`sudo readelf -a $TOMA_CRASH_FILE | grep nvmeibt_toma | head -1 | xargs`
		echo -e "${down_format}TOMA is Down${reset_format}\n${under_format}${big_under}${reset_format}\n${text_format}Core:\t${TOMA_CRASH_FILE}\nTime:\t${TOMA_CRASH_FILE_TIME}\nExec:\t${TOMA_CRASH_BIN}\n${reset_format}"
		TOMA_CRASH_BIN_SYMBOLS="$TOMA_CRASH_BIN.with_symbols"
		if [ -f $TOMA_CRASH_BIN_SYMBOLS ]; then
			alias tmcore="sudo gdb $TOMA_CRASH_BIN_SYMBOLS $TOMA_CRASH_FILE"
		else
			alias tmcore="sudo gdb $TOMA_CRASH_BIN $TOMA_CRASH_FILE"
		fi
	else
		echo -e "${down_format}TOMA is Down${reset_format}\n"
	fi
fi
# Check if client is up or down
if [ -d /sys/module/nvmeibc ]; then
	clientver=`cat /proc/nvmeibc/version`
	echo -e "${up_format}Client is Up${reset_format}${text_format} (${clientver})\n${under_format}${big_under}${text_format}"
	if [ -d /proc/nvmeibc/volumes ]; then
		for i in `ls -d /proc/nvmeibc/volumes/* 2>/dev/null`; do
			volname=`basename $i`
			volstat=`timeout $proc_timeout grep "Device status:" $i/status`
			voltype=`timeout $proc_timeout grep "Volume Type:" $i/status`
			echo -e "${subhead_format}Volume:${reset_format}${text_format}\t$volname\t$voltype\t$volstat"
		done
		echo -e "${under_format}${small_under}${text_format}"
	fi
	if [ -d /proc/nvmeibc/disks ]; then
		for i in `ls -d /proc/nvmeibc/disks/* 2>/dev/null`; do
			diskname=`basename $i`
			disknode=`timeout $proc_timeout grep "Node:" $i/status | xargs`
			diskstatus=`timeout $proc_timeout grep "Status:" $i/status | head -1 | xargs`
			echo -e "${subhead_format}Disk:${reset_format}${text_format}\t$diskname\t$diskstatus\t$disknode"
		done
	fi
	echo -e "${reset_format}"
else
	echo -e "${down_format}Client is Down${reset_format}\n"
fi
if [ -d /sys/module/nvmeib_keeper ]; then
	echo -e "${up_format}Keeper is Up${reset_format}\n"
	echo -e "${subhead_format}FRs:\n${text_format}$(cat /proc/nvmeib_keeper/frs | jq -r '.devices[] | .name as $dev | .instances[] | .inst_name as $inst | .frs[] | "\($dev): \($inst): \(.num_mrs)"')\n"
else
	echo -e "${down_format}Keeper is Down${reset_format}\n"
fi

# Check is target is up or down
if [ -d /sys/module/nvmeibs ]; then
	targetver=`cat /proc/nvmeibs/version`
	echo -e "${up_format}Target is Up ${reset_format}${text_format}($targetver)\n${under_format}${big_under}\n${subhead_format}Disks${reset_format}\n${under_format}${small_under}${text_format}"
	timeout $proc_timeout cat /proc/nvmeibs/disks.csv | column -t -s,
	echo -e "\n${subhead_format}NICs${reset_format}\n${under_format}${small_under}${text_format}"
	timeout $proc_timeout cat /proc/nvmeibs/nics.csv | column -t -s,
	echo -ne "${reset_format}"
	if [ -f /proc/nvmeibs/serjios.csv ]; then
        echo -e "\n${subhead_format}SERJIOs${reset_format}\n${under_format}${small_under}${text_format}"
        timeout $proc_timeout cat /proc/nvmeibs/serjios.csv | column -t -s,
    fi
    echo -ne "${reset_format}"
else
	echo -e "${down_format}Target is Down${reset_format}"
fi
