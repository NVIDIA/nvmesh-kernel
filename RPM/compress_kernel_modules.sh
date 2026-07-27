#!/bin/bash

function versionAbove7() {
	floorDistroVersion=`echo ${1} | cut -d"." -f1`

	if [ ${floorDistroVersion} -ge 7 ]; then return ; else false; fi
}

should_compress=false

if test -e /etc/redhat-release; then
	DISTRO_VER=`cat /etc/redhat-release | grep -Eo '[0-9]+\.[0-9]+'  | head -n1`

	if versionAbove7 $DISTRO_VER ; then
		should_compress=true
	fi
else
	DISTRIBUTION_INFO=`cat /etc/*release`
	if [[ "$DISTRIBUTION_INFO" =~ "Ubuntu" ]]; then
		should_compress=true
	fi
fi

if $should_compress; then
	#compress all .ko's
	echo "Compressing Kernel Modules..."
	if ! which xz > /dev/null 2>&1; then
		echo "ERROR: could not compress kernel modules, missing 'xz' tool"
		exit 1
	fi
		
	all_kos=`find . -name *.ko`
                
	if [ -z "$all_kos" ]; then
		echo "ERROR: could not find any .ko files"
		exit 1
	fi

	(xz --help | grep threads > /dev/null) && THREADS="--threads=0"
	for ko in $all_kos; do
		xz -f --check=crc32 ${THREADS} $ko
		if [ $? -ne 0 ]; then
			echo "ERROR: could not compress $ko with xz"
			exit 1
		fi
	done
fi
