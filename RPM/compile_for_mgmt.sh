#!/bin/bash

stty -g > /tmp/.currentTtySettings.$$
tty_backup=$(</tmp/.currentTtySettings.$$)

source compilator_utils.sh

exec 1> >(logger -s -t $(basename $0)) 2>&1

ubuntuDomainForPackaging='vm-mgmt-compilation-ubuntu-nodejs8_for_ron'

usage(){
        echo "USAGE: ./compile_for_mgmt.sh <VM_name>  <commit_ID> <branch> <output_dir> <private branch> <isUtils>"
}

if [ "$1" == "" ]; then
        echo "missing domain argument";
        usage;
        exit 1;
else
        if [ "$2" == "" ]; then
                echo "missing commit-ID argument";
                usage;
                exit 1;
        fi

        clone_domain="$1";
        commitID="$2";
        short_ID=${commitID:0:7};
        kvm_images_dir=/var/lib/libvirt/images/
	output_dir=$4
	isUtils=$6

        echo "commitID: "$commitID;
        echo "short commit id: "$short_ID;
	cd /tmp; git clone -b $3 git@gitlab.nvidia.com:$5/management.git; cd management;
	cd /tmp/management;
	git reset --hard $commitID

	sudo virsh start $clone_domain; ##starting cloned VM

        if [[ $? == 0 ]]; then ##If started succesfully
		establish_ssh_to_vm $clone_domain
                cloneIP=$IP

                echo Copying repo to VM.
                scp -rq /tmp/management nvmesh@$cloneIP:/tmp;

       		if [ "$isUtils" ];then
			pkg='utils'
                	ssh -t nvmesh@$cloneIP "ls -l /tmp;  ls -l /tmp/management; cd /tmp/management; cd RPM; ls -l .; ./makeExecutable.sh; exit;";
		else
			pkg='mgmt'
			ssh -t nvmesh@$cloneIP "ls -l /tmp;  ls -l /tmp/management; cd /tmp/management; pwd; sudo npm install -g apidoc;apidoc -i routes -o docs; npm install --production; cd RPM; sudo ./buildrpm -M; exit;";
		fi

                ret=$?

               if [[ $ret != 0 ]]; then
                        echo "ERROR while compiling the code"
                        shutdown_clone_and_exit $clone_domain
               fi

               echo copying compiled installation files;
               echo "Destination directory: $output_dir"
               srv_path="${output_dir}/${commitID}_${pkg}/"
               mkdir -p $srv_path
	       echo "mkdir -p ${srv_path}"

	       if [ "$isUtils" ];then
		        echo '----------- making nvmesh-utils packages'
	                scp -r nvmesh@$cloneIP:/tmp/management/NVMeshCLI/dist /tmp/management/NVMeshCLI;

		       	sudo virsh start $ubuntuDomainForPackaging
                        establish_ssh_to_vm $ubuntuDomainForPackaging
		        ubuntuIP=$IP

                        echo 'copying repo to the ubuntu VM'
	                scp -rq /tmp/management nvmesh@$ubuntuIP:/tmp;
                        echo 'packaging executables on the ubuntu VM'
	                ssh -t nvmesh@$ubuntuIP "ls -l /tmp;  ls -l /tmp/management; cd /tmp/management; cd RPM; ls -l .; ./buildrpm -e -u; exit;";

                        ret=$?
                        if [[ $ret != 0 ]]; then
                                echo "ERROR while packaging on the ubuntu VM"
                                shutdown_clone_and_exit $ubuntuDomainForPackaging
                        fi

                        echo 'copying the utils packages from the ubuntu VM'
                        scp nvmesh@$ubuntuIP:/tmp/management/RPM/*-utils* $srv_path/;
                        ssh -t nvmesh@$ubuntuIP "sudo rm -rf /tmp/management; exit"
                        shutdown_clone $ubuntuDomainForPackaging
                else
                        scp nvmesh@$cloneIP:/tmp/management/RPM/*-management* $srv_path/;
                fi

                ssh -t nvmesh@$cloneIP "sudo rm -rf /tmp/management; exit"
                shutdown_clone $clone_domain
	        sudo rm -rf /tmp/management;

	       echo "The installation files can now be found at ${srv_path}"
        else
                echo "VM Didnt Start";
                exit 1;
        fi
fi
