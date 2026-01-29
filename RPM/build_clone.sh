#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

USAGE_STR="USAGE: ./build_clone.sh -c <clone_name> -i <base image name> -k <kernel> -o <ofed path>"
UB_INBOX_DEPENDENCIES="libibverbs1 librdmacm1 libibcm1 libibmad5 libibumad3 libmlx4-1 libmlx5-1 opensm ibutils infiniband-diags perftest mstflint rdmacm-utils ibverbs-utils gcc rsync"
EL_INBOX_DEPENDENCIES="libibverbs-devel libibmad-devel libibcm-devel opensm-devel librdmacm-devel ibutils-devel libmlx5 libmlx4 infiniband-diags perftest librdmacm-utils libibverbs-utils libibumad gcc rsync libudev-devel"
UB_DEPENDENCIES="autoconf"
EL_DEPENDENCIES="autoconf"
stty -g > /tmp/.currentTtySettings.$$
tty_backup=$(</tmp/.currentTtySettings.$$)
# Functions:
#===============================================================================

# # Checks if the last command before the function call was successful.
# # If not, prints the appropriate message and exits on the right error code if needed.
# Arguments:
#   error message string
#   exit code (if there is no need to exit pass 0 as an error code)
# Returns:
#   None
function checkLastCommand {
    local lastCmdExitCode=$?
    local errMsg=$1
    local errCode=$2

    # Checks if the last command failed
    if [ ${lastCmdExitCode} -ne 0 ]; then
        # Checks if there is an error code to be exited on, if not just prints the error message
        if [ ${errCode} -ne 0 ]; then
            echo ${errMsg} 1>&2
            #cleanBase
            exit ${errCode}
        else
            echo ${errMsg}
        fi
    fi
}
#===============================================================================
# # Checking when the VM is completely shut down
# Arguments:
#   VM name
# Returns:
#   None
function is_VM_shut {
    local vm_name=$1
    local check_VM_state=0
    local spin='-\|/'
    local i=0
    echo "${vm_name} is shutting down"
    until [ ${check_VM_state}  -eq 1 ]; do
        i=$(( (i+1) %4 ))
        printf "\r${spin:$i:1}"
        sleep .1
        sudo virsh list | grep ${vm_name} 1>/dev/null 2>/dev/null
        check_VM_state=$?
    done
}
#===============================================================================
number_of_occurrences() { ##of substring in string
	str="$1"
	sub="$2"
	grep -o "$sub" <<< "$str" | wc -l
}


virt-addr() { ##ip of virtual machine
	domain="$1"
	arp -an | grep "`sudo virsh dumpxml "$domain" | grep "mac address" | sed "s/.*'\(.*\)'.*/\1/g"`" | awk '{ gsub(/[\(\)]/,"",$2); print $2 }'
}

usage(){
	echo ${USAGE_STR}
}

checkssh(){
	echo waiting for sshd service...
	sleep 2
	ssh nvmesh@$cloneIP exit;
	sshTest=$?;
	echo sshTest: $sshTest;
	if [[ $sshTest != 0 ]]; then
		until [[ $sshTest == 0 ]]; do
			sleep 1;
			ssh -t nvmesh@$cloneIP exit;
			sshTest=$?;
			echo sshTest: $sshTest;
		done
		echo ssh ready
	fi
}


check_if_inbox_driver_dependencies_installed(){
	distro=$1
	if [ ${distro} -eq 1 ]; then
		cmd="dpkg -l | grep -E 'libibverbs1|librdmacm1|libibcm1|libibmad5|libibumad3|libmlx4-1|libmlx5-1|opensm|ibutils|infiniband-diags|rdmacm-utils|ibverbs-utils' | wc -l | grep 12"
	elif [ ${distro} -eq 2 ]; then
		cmd="rpm -qa | grep -E 'libibmad-devel|opensm-devel|ibutils-devel|infiniband-diags|librdmacm-utils|libibverbs-utils|libibumad' | wc -l | grep 9"
	fi
	ssh -t nvmesh@$cloneIP $cmd 
	return $?
}

check_if_package_installed(){
	pkg=$1
	distro=$3
	if [ ${distro} -eq 1 ]; then
		cmd="dpkg -l | grep ${pkg}"
	elif [ ${distro} -eq 2 ]; then
		cmd="rpm -qa | grep ${pkg}"
	fi
	ssh -t nvmesh@$cloneIP $cmd
	ret=$?

	if [ "$ret" != "0" ] && [ "$2" == "exit" ]; then
		echo "ERROR: ${pkg} was not installed"
		shutdown_clone_and_exit $clone_domain
	fi

	return $ret
}

shutdown_clone_and_exit() {
	shutdown_clone $1
	exit 1
}

shutdown_clone() {
	stty $tty_backup
	sudo virsh shutdown $1
}

install_new_packages_and_exit() {
	 echo "Installing this packages: ${pkgs_to_add}"
         ssh -t nvmesh@${cloneIP} "sudo ${pkg_install_cmd} -y ${pkgs_to_add}"
         checkLastCommand "Unable to install ${pkgs_to_add}, exiting" 1
         shutdown_clone $clone_domain
	 exit 0
}

kvm_images_dir=/var/lib/libvirt/images/

# Parse input options
while getopts "c:i:k:o:hu:" opt; do
  case ${opt} in
    c )
      clone_domain=("$OPTARG")
      ;;
    h )
	  echo -e ${USAGE_STR}
      exit 0
      ;;
    i )
      image_name=("$OPTARG")
      ;;
    k )
      kernel=("$OPTARG")
      ;;
    o )
      ofed_path=("$OPTARG")
      ;;
    u )
      pkgs_to_add=("$OPTARG")
      ;;    
    \? )
      echo "Invalid option: $OPTARG" 1>&2
      ;;
    : )
      echo "Invalid option: $OPTARG requires an argument" 1>&2
      ;;
  esac
done
shift $((OPTIND -1))

	ofed_tgz=${ofed_path##*/} ##ofed name

	sudo virsh start $clone_domain; ##starting cloned VM
	
	if [[ $? == 0 ]]; then ##If started succesfully
		echo "waiting for the IP of the clone..."
		cloneIP=$(virt-addr $clone_domain); ##get the IP of cloned VM
		echo "IP: $cloneIP";
		ipDotsNum=$(number_of_occurrences "$cloneIP" "\."); ##number of dots (".") in the received IP

		if [[ $ipDotsNum > 3 ]]; then ##got more thqan one IP address
			echo "Invalid IP. more than 3 dots. probably gave more than 1 IP. probably bad clone Domain Name. received as ip: $cloneIP";
			shutdown_clone_and_exit $clone_domain 
		fi
		
		until [[ $ipDotsNum  == 3 ]]; do ##wait until a valid IP is received
			sleep 1;
			cloneIP=$(virt-addr $clone_domain)
			echo "IP: $cloneIP";
			ipDotsNum=$(number_of_occurrences "$cloneIP" "\.");
		done

		bash -c "ssh-keyscan $cloneIP >> ~/.ssh/known_hosts"; ##to avoid prompting for yes/no on append to known_hosts			
		checkssh

	    	
		#Check distibution type (currently ub/el supported) and store it in $distro
		# Ubuntu 	 ->	$distro = 1 
		# CentOS/Red Hat -> 	$distro = 2
		distVersion=`ssh -t nvmesh@${cloneIP} "cat /proc/version"`
		echo ${distVersion} | grep "ubuntu" -i
		if [ $? -eq 0 ]; then distro=1; fi
		echo ${distVersion} | grep "red hat" -i
		if [ $? -eq 0 ]; then distro=2; fi

		#Prepare installation commands according to $distro
		if [ ${distro} -eq 1 ]; then 
			pkg_install_cmd="apt install";
			kernel_pkg="linux-image-"; 
			kernel_dev_pkg="linux-headers-";
			inbox_driver_pkgs=${UB_INBOX_DEPENDENCIES}
			extra_pkgs=${UB_DEPENDENCIES}
			if [ "$pkgs_to_add" != "none" ]; then install_new_packages_and_exit ;fi
			ssh -t nvmesh@${cloneIP} "sudo ${pkg_install_cmd} -y ${kernel_pkg}${kernel} ${kernel_dev_pkg}${kernel}"
                        checkLastCommand "Unable to install ${kernel_pkg}${kernel} and/or ${kernel_dev_pkg}${kernel} , exiting" 0
		elif [ ${distro} -eq 2 ]; then 
			pkg_install_cmd="yum install"; 
       		        kernel_pkg="kernel-";
                        kernel_dev_pkg="kernel-devel-";
                        inbox_driver_pkgs=${EL_INBOX_DEPENDENCIES}
                        extra_pkgs=${EL_DEPENDENCIES}
			if [ "$pkgs_to_add" != "none" ]; then install_new_packages_and_exit ;fi
			
			current_kernel=`ssh -t nvmesh@${cloneIP} 'uname -r'`
			
			if [ $kernel != $current_kernel ]; then
				ssh -t nvmesh@${cloneIP} "sudo ${pkg_install_cmd} wget -y"
				checkLastCommand "Unable to install wget, exiting" 0
	
				ssh -t nvmesh@${cloneIP} "sudo wget http://ftp.riken.jp/Linux/cern/centos/7/updates/x86_64/Packages/${kernel_pkg}${kernel}.rpm"
				checkLastCommand "Unable to download kernel ${kernel}, exiting" 0
	
				ssh -t nvmesh@${cloneIP} "sudo ${pkg_install_cmd} ${kernel_pkg}${kernel}.rpm -y"
				checkLastCommand "Unable to install ${kernel_pkg}${kernel}.rpm, exiting" 0
	
				ssh -t nvmesh@${cloneIP} "sudo wget http://ftp.riken.jp/Linux/cern/centos/7/updates/x86_64/Packages/${kernel_dev_pkg}${kernel}.rpm"
				checkLastCommand "Unable to download ${kernel_dev_pkg}${kernel}, exiting" 0
	
				ssh -t nvmesh@${cloneIP} "sudo ${pkg_install_cmd} ${kernel_dev_pkg}${kernel}.rpm -y"
				checkLastCommand "Unable to install ${kernel_dev_pkg}${kernel}.rpm, exiting" 0
			else
				echo "This kernel: $kernel is already installed!"
			fi

		fi

		check_if_package_installed $kernel_pkg$kernel 'exit' $distro
		check_if_package_installed $kernel_dev_pkg$kernel 'exit' $distro

		shutdown_clone ${clone_domain}
		is_VM_shut ${clone_domain}
		sudo virsh start ${clone_domain}
        	sleep 20

		echo ${ofed_path} | grep --quiet "none"
		if [ "$ofed_tgz" != "none"  ]; then
			echo Installing OFED.;
			echo ofed_path: $ofed_path;

			if [[ ! -f $ofed_path ]]; then
				echo Specified ofed_tgz path not found. Aborting.
				shutdown_clone_and_exit $clone_domain
			fi
			sudo ./ofed_install.sh -t ${cloneIP} -p ${ofed_path}
			stty $tty_backup
		else
			ssh -t nvmesh@${cloneIP} "sudo ${pkg_install_cmd} -y ${inbox_driver_pkgs}"
		    	check_if_inbox_driver_dependencies_installed $distro
		    	pkgs_installed=$?

		    	if [[ $pkgs_installed != 0 ]]; then
		        	shutdown_clone_and_exit $clone_domain
		    	fi
		fi
		stty $tty_backup
	        ssh -t nvmesh@${cloneIP} "sudo ${pkg_install_cmd} -y ${extra_pkgs}"
		shutdown_clone $clone_domain
	else
		echo "VM Didn't Start";
		exit 1;
	fi
