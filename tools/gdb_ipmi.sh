#!/bin/sh

if [ $# -lt 2 ] || [ $# -gt 4 ]; then
	echo "$0 <nvme_num> <symbols: tgt/clnt,none> <nvmesh_build_dir> <kver>"
	exit -1
fi

nvme=$1
symbols=$2

if [ $nvme -lt 255 ]; then
	IP=10.0.1.$nvme
        IPMI_IP=10.0.2.$nvme
else
        if [ $nvme -gt 1000 ]; then
		IP=10.0.11.$(($nvme - 1000))
                IPMI_IP=10.0.12.$(($nvme - 1000))
        else
                echo Invalid nvme $nvme
        fi
fi

LB_IP="127.0.0.1"
LB_PORT="$((${RANDOM} + 1024))"
SSH_TO="2s"

if [ $# -lt 3 ]; then
	NVMESH_BUILD_DIR="${HOME}/src/ssda"
fi

if [ $# -lt 4 ]; then
	KVER=`uname -r`
fi

# Start SOCAT
socat TCP4-LISTEN:$LB_PORT,reuseaddr,fork EXEC:"ipmitool -I lanplus -H $IPMI_IP -U ADMIN -P ADMIN sol activate" &
SOCAT_PID=$!

#Create gdbinit script
GDBINIT_TMP_FILE=`mktemp`
echo "set architecture i386:x86-64:intel
target remote $LB_IP:$LB_PORT
dir /lib/debug/lib/modules/$KVER/
" > $GDBINIT_TMP_FILE

if [ "$symbols" = "tgt" ]; then
	#SSH into host to get module locations
	NVMEIB_SECTIONS_STR=`ssh $IP "cat /sys/module/nvmeibs/sections/.text /sys/module/nvmeibs/sections/.data /sys/module/nvmeibs/sections/.bss /sys/module/nvmeibc/sections/.text /sys/module/nvmeibc/sections/.data /sys/module/nvmeibc/sections/.bss /sys/module/nvmeib_common/sections/.text /sys/module/nvmeib_common/sections/.data /sys/module/nvmeib_common/sections/.bss | tr '\n' ' '"`
	read -r -a NVMEIB_SECTIONS <<< $NVMEIB_SECTIONS_STR
	NVMEIBS_TEXT="${NVMEIB_SECTIONS[0]}"
	NVMEIBS_DATA="${NVMEIB_SECTIONS[1]}"
	NVMEIBS_BSS="${NVMEIB_SECTIONS[2]}"
	NVMEIBC_TEXT="${NVMEIB_SECTIONS[3]}"
	NVMEIBC_DATA="${NVMEIB_SECTIONS[4]}"
	NVMEIBC_BSS="${NVMEIB_SECTIONS[5]}"
	NVMEIB_COMMON_TEXT="${NVMEIB_SECTIONS[6]}"
	NVMEIB_COMMON_DATA="${NVMEIB_SECTIONS[7]}"
	NVMEIB_COMMON_BSS="${NVMEIB_SECTIONS[8]}"
	echo "add-symbol-file $NVMESH_BUILD_DIR/srv/nvmeibs.ko $NVMEIBS_TEXT -s .data $NVMEIBS_DATA -s .bss $NVMEIBS_BSS
add-symbol-file $NVMESH_BUILD_DIR/clnt/nvmeibc.ko $NVMEIBC_TEXT -s .data $NVMEIBC_DATA -s .bss $NVMEIBC_BSS
add-symbol-file $NVMESH_BUILD_DIR/common/nvmeib_common.ko $NVMEIB_COMMON_TEXT -s .data $NVMEIB_COMMON_DATA -s .bss $NVMEIB_COMMON_BSS
" >> $GDBINIT_TMP_FILE
elif [ "$symbols" = "clnt" ]; then
	#SSH into host to get module locations
        NVMEIB_SECTIONS_STR=`ssh $IP "cat /sys/module/nvmeibc/sections/.text /sys/module/nvmeibc/sections/.data /sys/module/nvmeibc/sections/.bss /sys/module/nvmeib_common/sections/.text /sys/module/nvmeib_common/sections/.data /sys/module/nvmeib_common/sections/.bss | tr '\n' ' '"`
        read -r -a NVMEIB_SECTIONS <<< $NVMEIB_SECTIONS_STR
        NVMEIBC_TEXT="${NVMEIB_SECTIONS[0]}"
        NVMEIBC_DATA="${NVMEIB_SECTIONS[1]}"
        NVMEIBC_BSS="${NVMEIB_SECTIONS[2]}"
        NVMEIB_COMMON_TEXT="${NVMEIB_SECTIONS[3]}"
        NVMEIB_COMMON_DATA="${NVMEIB_SECTIONS[4]}"
        NVMEIB_COMMON_BSS="${NVMEIB_SECTIONS[5]}"
	echo "add-symbol-file $NVMESH_BUILD_DIR/clnt/nvmeibc.ko $NVMEIBC_TEXT -s .data $NVMEIBC_DATA -s .bss $NVMEIBC_BSS
add-symbol-file $NVMESH_BUILD_DIR/common/nvmeib_common.ko $NVMEIB_COMMON_TEXT -s .data $NVMEIB_COMMON_DATA -s .bss $NVMEIB_COMMON_BSS
" >> $GDBINIT_TMP_FILE
fi

#cat $GDBINIT_TMP_FILE

# Enable kgdboc on host
ssh $IP "echo ttyS1,115200 | sudo tee /sys/module/kgdboc/parameters/kgdboc"

# Enter debug on host
timeout $SSH_TO ssh $IP "echo g | sudo tee /proc/sysrq-trigger"

# Run GDB
gdb -tui /lib/debug/lib/modules/$KVER/vmlinux -x $GDBINIT_TMP_FILE

# Kill SOCAT
kill $SOCAT_PID
