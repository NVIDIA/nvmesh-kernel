#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set -x
oldest_GLIBC_domain_for_pyinstaller='c6.4m-o4.2-1.0.0.0-k2.6.32-696.6.3.el6_base_for_pyinstaller_executables'

source compilator_utils.sh

SERVICES='managementCM.py managementAgent.py'
TMP_MGMT_CM='/tmp/nvmesh/management_cm'

make_exes() {
    SERVICES='managementCM.py managementAgent.py'
    echo "services: $SERVICES"
    exportLdLib="export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib"
    $exportLdLib
    python27=`which python2.7`
    isPyinstallerInstalled=`$python27 -c 'import PyInstaller' ; echo $?`
    
    if [ $isPyinstallerInstalled -ne 0 ];then
    	pip27=`which pip2.7`
        echo 'trying to install pyinstaller'
        $pip27 install pyinstaller==3.4
        
        if [ $? -ne 0 ];then
            echo "PyInstaller is not installed, can not make executables, exiting"
            exit 1
        fi
    fi
    
	pathToSitePkgs='/usr/local/lib/python2.7/site-packages'
	cd /tmp/management_cm
	sudo mkdir exeServices
	for service in $SERVICES; do
		distPath=${service%.py}_dist
		sudo -E bash -c "$exportLdLib ; pyinstaller $service --paths=$pathToSitePkgs --distpath $distPath -y"
		sudo mv $distPath exeServices
	done
}

sudo virsh start $oldest_GLIBC_domain_for_pyinstaller
establish_ssh_to_vm $oldest_GLIBC_domain_for_pyinstaller
pyinstaller_vm_ip=$IP

echoStdout "Copying management_cm to the PyInstaller VM in order to create executables from the management services"
scp -rq $TMP_MGMT_CM nvmesh@${pyinstaller_vm_ip}:/tmp;

echoStdout "running makeExecutables.sh in the PyInstaller VM"
ssh -t nvmesh@${pyinstaller_vm_ip} "ls -l /tmp;  ls -l /tmp/management_cm; cd /tmp/management_cm; $(declare -f make_exes); make_exes ; exit $?;";

ret=$?
if [[ $ret != 0 ]]; then
		echoStderr "ERROR while trying to make executables from the management services on $pyinstaller_vm_ip"
		shutdown_clone_and_exit $pyinstaller_vm_ip
fi

echo 'copying the executables from the PyInstaller VM'
scp -rq nvmesh@$pyinstaller_vm_ip:/tmp/management_cm/exeServices $TMP_MGMT_CM

mkdir -p $TMP_MGMT_CM/src

for service in $SERVICES; do 
	mv -f $TMP_MGMT_CM/exeServices/${service%.py}_dist/${service%.py}/* $TMP_MGMT_CM/exeServices
	rm -rf $TMP_MGMT_CM/exeServices/${service%.py}_dist/
	mv -f $TMP_MGMT_CM/$service $TMP_MGMT_CM/src
done
ssh -t nvmesh@$pyinstaller_vm_ip "sudo rm -rf /tmp/management_cm; exit"
shutdown_clone $oldest_GLIBC_domain_for_pyinstaller
