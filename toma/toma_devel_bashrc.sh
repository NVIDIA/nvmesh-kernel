# .bashrc
# Source global definitions
shopt -s expand_aliases
shopt -s extglob

ulimit -c unlimited

# Slickedit compilation error parsing
# primary:	 		^{#0[^:b]+:p}\:{#1[0-9]+:i}\:{#2[0-9]+:i}\: (fatal|error|warning|note)\: {#3?+}$
# primary link error:		^{#0:p}\:{#1:i}\: (multiple|undefined){#3?+}$
# In file included from:	^In:bfile:bincluded:bfrom:b{#0:p}\:{#1:i}\:{#2:i}\,:b{#3?+}$
# From:				^:b:b:b:b:b:b:b:b:b:b:b:b:b:b:b:b:bfrom:b{#0:p}\:{#1:i}\,:b{#3?+}$
# Backup of primary:		^{#0:p}\:{#1:i}\:{#2:i}\: (fatal|error|warning|note)\: {#3?+}$

################################ From https://confluence.nvidia.com/pages/viewpage.action?spaceKey=NSVSREC&title=NSVSRECS+Onboarding ###############################
# Setting for Nvidia Vault
export VAULT_ADDR=https://prod.vault.nvidia.com
export VAULT_NAMESPACE=ngc

# Alias for easy vault login
alias vault_login='vault login -method=oidc -path=oidc role=ngc'

# Or a combination if you use multiple namespaces
alias vault_login_ngc="export VAULT_NAMESPACE=ngc; vault login -method=oidc -path=oidc role=ngc"

############################### From https://confluence.nvidia.com/display/NSVSREC/NVInit+For+Accessing+SwiftStack+Systems #################################

MTV_EXCELERO1='mtv-excelero1.mec01.nbulabs.nvidia.com'
MTV_EXCELERO1='mtv-excelero1'

# In your ~/.bashrc or equiv
NVIDIA_USERNAME=`whoami`
alias nvssh='nvinit ssh -user $NVIDIA_USERNAME -vault-role sshca-usercert/issue/ss-legacy -principals "${NVIDIA_USERNAME},bouncer";

# Run nvssh to fetch key
$nvinit ssh -user $NVIDIA_USERNAME'
$nvssh

# nvinit ssh ronen
# ssh-add -L | ssh-keygen -L -f -
# ssh in-c242-n2

##################################################################################################

if [[ "`hostname`" =~ nvme.* ]]; then
	#################################################################################################################################
	###########################################      THE SERVER (not-my-laptop) code     ############################################
	#################################################################################################################################
	tabs=' 									'
	alias unbuffered="stdbuf -i0 -o0 -e0"
	alias trigger_crash='sudo bash -c "echo c > /proc/sysrq-trigger"'
	alias valgrind_toma="sudo valgrind --suppressions=./scripts/nvmeibt_valgrind_suppress.conf --max-stackframe=2483352 ./bin/release/nvmeibt_toma.with_symbols"
	alias restore_mgt_db='sudo mongo management /opt/nvmesh/management/clearDB.js; mongorestore -d management ~/mgt_dump/management/'
	alias dropDB='sudo mongo management --eval "db.dropDatabase()"'
	alias save_mgt_db='mongodump -d management -o mgt_dump'
	alias toggle_ib_nvme43='port43=`sudo iblinkinfo | grep nvme43 | grep == | head -1 | cut -d"[" -f 1` ; while true; do sudo ibportstate $port43 disable; echo --DOWN\?--; sleep 20; sudo ibportstate $port43 enable; echo --UP\?--; sleep 20; done'
	alias toggle_networking='cd ~/projects/ssda/tools; ./toggle_ib_port.py --switchAddress 10.0.4.3 --port "1/3" --dt 1 --ut 300 & ./toggle_ib_port.py --switchAddress 10.0.4.3 --port "1/4" --dt 1 --ut 100 & ./toggle_ib_port.py --switchAddress 10.0.4.3 --port "1/5" --dt 1 --ut 50 &'
	#run
	alias my_gdb_OLD='cd ~/projects/ssda/toma; sudo gdb bin/udp/nvmeibt_toma `ls -t core* | head -1`'
	alias my_gdb='cd ~/projects/ssda/toma; sudo lz4 -d -f `ls -t /var/lib/systemd/coredump/core* | head -1` core; sudo gdb bin/*/nvmeibt_toma.with_symbols core*'
	alias rebind_lost_drives_to_nvme='for j in /sys/class/pci_bus/0000\:*/device/ ; do echo === ${j} === ; (cd ${j} ; for i in 000* ; do echo ---- ${i} ---- ; sudo bash -c "echo ${i} > /sys/bus/pci/drivers/nvmeibs/unbind; echo ${i} > /sys/bus/pci/drivers/nvme/bind"; done) ; done'
	alias nvme_orphans='devices_path=/sys/bus/pci/devices; for pci in $(lspci -Dd ::0108 | cut -f1 -d " "); do if [ ! -d "$devices_path/$pci/driver" ]; then echo "$pci has no driver, bind it back to nvme inbox driver"; sudo bash -c "echo -n $pci > /sys/bus/pci/drivers/nvme/bind"; fi; done'
	#kafka
	function _run_kafka_server {
		pushd ~/kafka-3.2.0-src/;
		bin/zookeeper-server-start.sh config/zookeeper.properties |& tee zookeeper.stdout &
		sleep 5;
		bin/kafka-server-start.sh config/server.properties |& tee kafka-server-start.stdout &
		popd;
	}
	alias run_kafka_server=_run_kafka_server
	alias OLD_dump_kafka_logs='~/kafka-3.2.0-src/bin/kafka-dump-log.sh --print-data-log --files /tmp/kafka-logs/zone1.leader.incrementalUpdates-0/00000000000000000000.log'
	alias dump_kafka_logs='/opt/kafka_2.12-3.2.0/bin/kafka-dump-log.sh --print-data-log --files /var/lib/kafka/zone1.leader.incrementalUpdates-0/00000000000000000000.log'
	alias dump_topics='~/kafka-3.2.0-src/bin/kafka-configs.sh --describe --bootstrap-server localhost:9092 --entity-type topics --all'
	#
	alias dd_test='
		if ( ! `lsmod | cut -d" " -f1 | grep -q nvmeibc` ) ; then
			echo "No nvmeibc" ;
		else
			cd ~/projects/ssda/toma;
			sudo bash -c "echo 2 > /sys/module/nvmeibc/parameters/debug_level";
			x=-1;
			J1=/dev/nvmesh/J1;
			R1=/dev/nvmesh/R1;
			while true; do
				x=$(($x+1));
				if ((${x}%10==0)); then
					/usr/bin/nvmesh client detach --id `hostname` --volume R1
					/usr/bin/nvmesh client detach --id `hostname` --volume J1
					/usr/bin/nvmesh client attach --id `hostname` --volume R1
					/usr/bin/nvmesh client attach --id `hostname` --volume J1
				fi;
				echo "`date +%H:%M:%S` $x ${tabs:0:$(($x%7+1))}*****";
				if ([ ! -b $J1 ] || [ ! -b $R1 ]); then
					echo no-dev;
					timeout 10 read -n 1;
				else
					sudo dd bs=64k if=$J1 of=$R1 iflag=direct oflag=direct count=1026 conv=nocreat && sleep 1 || ( echo DETACHING ALL;  ~/nvmesh client detach --id `hostname`--force --all; if ([ ! -b $R1 ]); then sudo rm $R1 ; fi );
				fi;
			done ;
		fi
	'
	alias ec_test='
		if ( ! `lsmod | cut -d" " -f1 | grep -q nvmeibc` ) ; then
			echo "No nvmeibc" ;
		else
			cd ~/projects/ssda/toma;
			sudo bash -c "echo 2 > /sys/module/nvmeibc/parameters/debug_level";
			x=-1;
			EC1=/dev/nvmesh/EC1;
			R1=/dev/nvmesh/R1;
			while true; do
				x=$(($x+1));
				if ((${x}%10==0)); then
					~/nvmesh client detach --force --id `hostname` --volume R1
					~/nvmesh client detach --force --id `hostname` --volume EC1
					~/nvmesh client attach --id `hostname` --volume R1
					~/nvmesh client attach --id `hostname` --volume EC1
				fi;
				echo "`date +%H:%M:%S` $x ${tabs:0:$(($x%7+1))}*****";
				if ([ ! -b $EC1 ] || [ ! -b $R1 ]); then
					echo no-dev;
					timeout 10 read -n 1;
				else
					sudo dd bs=64k if=$EC1 of=$R1 iflag=direct oflag=direct count=1026 conv=nocreat && sleep 1 || ( echo DETACHING ALL;  ~/nvmesh client detach --id `hostname`--force --all; if ([ ! -b $R1 ]); then sudo rm $R1 ; fi );
				fi;
			done ;
		fi
	'
	alias bt_test='
		host_idx=-1;
		if [ "`hostname`" == "nvme42.excelero.com" ] || [ "`hostname`" == "nvme114.excelero.com" ]; then
			host_idx=0;
		elif [ "`hostname`" == "nvme43.excelero.com" ] || [ "`hostname`" == "nvme116.excelero.com" ]; then
			host_idx=1;
		elif [ "`hostname`" == "nvme44.excelero.com" ] || [ "`hostname`" == "nvme122.excelero.com" ]; then
			host_idx=2;
		fi;
		if ( ! `lsmod | cut -d" " -f1 | grep -q nvmeibc` ) ; then
			echo "No nvmeibc" ;
		else
			cd ~/projects/ssda/toma;
			sudo bash -c "echo 2 > /sys/module/nvmeibc/parameters/debug_level";
			x=-1;
			R1=/dev/nvmesh/R1;
			while true; do
				x=$(($x+1));
				if ((x%10==0)); then
					sudo nvmesh client detach --id `hostname` --volume R1 --force;
					sudo nvmesh client attach --id `hostname` --volume R1;
				fi;
				echo "`date +%H:%M:%S` $x ${tabs:0:$(($x%7+1))}*****";
				if ([ ! -b $R1 ]); then
					echo no-dev;
					timeout 10 read -n 1;
				else
					sudo /opt/nvmesh/perfTest/io_stress//btestEX -d -c -j ${host_idx},3 -q -t 0 -T 2 -w 8 -D -B 300000 R 50 ${R1} && sleep 1 || ( echo REMOVING $R1; sudo sudo nvmesh client detach --id `hostname` --volume R1 --force; if ([ ! -b $R1 ]); then sudo rm $R1 ; fi );
				fi;
			done ;
		fi
	'
	alias rm_old_dict_files='(cd ~/projects/ssda; for dir in "toma/trace/nvmeibt_toma/debug/" "toma/trace/nvmeibt_toma_replay/debug/" "tools/pipe_tracer/build/" ; do echo ${dir}; ls -t ${dir}/dict\.[0-9][0-9]*\.json | tail --lines=+2 | xargs rm -f ; done;)'
	alias run_toma='
		sudo pkill -9 nvmeibt_toma; cd ~/projects/ssda/toma; rm -f core core\.[0-9]*; rm_old_dict_files;
		(cd /var/log/nvmesh/trace_daemon/; sudo rm dict\.[0-9]*\.json; sudo tar --no-same-owner -xvpSf ~/projects/ssda/dictionaries.tar.gz); sleep 2;
		sudo bash -c "echo \"+ all\" > /var/log/nvmesh/toma_trace.config";
		(cd ~/projects/ssda/toma ; sudo bash -c "source /etc/nvmesh/nvmesh.conf; export KAFKA_SERVERS; ./bin/release/nvmeibt_toma -s 4G --nm-transport ./bin/release/nw_ibud.so");
	'
	alias run_toma_udp='
		sudo pkill -9 nvmeibt_toma; cd ~/projects/ssda/toma; rm -f core core\.[0-9]*; rm_old_dict_files;
		(cd /var/log/nvmesh/trace_daemon/; sudo rm dict\.[0-9]*\.json; tar --no-same-owner -xvpSf ~/projects/ssda/dictionaries.tar.gz); sleep 2;
		sudo ./bin/udp/nvmeibt_toma -s 4G --nm-transport ./bin/udp/nw_ibud.so;
	'
	tirtur_params='-rt 100 -dt 5'; if [ "$HOSTNAME" == "nvme1006.excelero.com" ]; then tirtur_params='-rt 1000 -dt 3'; elif [ "$HOSTNAME" == "nvme1015.excelero.com" ]; then tirtur_params='-rt 50 -dt 4'; fi
	alias run_tirtur='
		cd ~/projects/ssda/toma;
		pushd /var/log/nvmesh/trace_daemon/; rm -f core core\.[0-9]*; sudo rm dict\.[0-9]+\.json; tar --no-same-owner -xvpSf ~/projects/ssda/dictionaries.tar.gz; popd
		./toma_TIRTUR.sh '"$tirtur_params"
	alias reinstall_nvmesh-utils_here='
		OS_version=`cat /etc/system-release| cut -f6 -d" " | cut -d"." --output-delimiter="_" -f1,2`
		#deb_file=`ssh nvme1013 ls -tr /data/management-rpms/management_master_HEAD/utils/nvmesh-utils\*amd64.deb | tail -1`;
		rpm_file=`ssh nvme1013 ls -tr /data/management-rpms/management_master_HEAD/utils/nvmesh-utils-+\(\[0-9\]\).+\(\[0-9\]\).+\(\[0-9\]\)-+\(\[0-9\]\).el8*.x86_64.rpm | grep -v ubuntu | tail -1`;
		rpm_file=`ssh nvme1013 ls -tr /data/management-rpms/management_master_*/utils/nvmesh-utils-+\(\[0-9\]\).+\(\[0-9\]\).+\(\[0-9\]\)-+\(\[0-9\]\).el${OS_version}.x86_64.rpm | grep -v ubuntu | tail -1`;
		#rm nvmesh-utils*amd64.deb;
		rm nvmesh-utils*x86_64.rpm;
		#scp nvme1013:${deb_file} .
		scp nvme1013:${rpm_file} .
		#sudo dpkg -i --force-overwrite ./nvmesh-utils*amd64.deb || return 1;
		sudo yum install -y ./nvmesh-utils*x86_64.rpm || return 1;
	'
	alias run_full_init='function __run_full_init() {
		mkdir /tmp/backup; cp -p /var/opt/nvmesh/toma/toma_persistence_*_raft_and_topo.0 /tmp/backup/
		scp ${MTV_EXCELERO1}:/usr/local/lib/infra/infra-bin/nvmesh ~/	# Used for attach/detach
		removed_dict_files=`ls -t ~/projects/ssda/toma/trace/nvmeibt_toma/debug/dict\.[0-9]*\.json | tail -n +2`
		removed_dict_files_2=`ls ~/projects/ssda/toma/trace/nvmeibt_toma_replay/debug/dict\.[0-9]*\.json`;
		if [ "_${removed_dict_files}" != "_" ]; then
			echo removing ${removed_dict_files} ${removed_dict_files_2};
			sudo rm -rf ${removed_dict_files} ${removed_dict_files_2};
		fi
		sudo rm -rf /var/log/nvmesh/*; sudo mkdir /var/log/nvmesh/trace_daemon
		cd ~/projects/ssda/toma;
		~/nvmesh client detach --force --id `hostname` --all
		#sudo apt-get --purge -y remove nvmesh-core
		sudo yum remove -y nvmesh-target;
		sudo rmmod nvmeiba nvmeibs nvmeibc nvmeib_common_mlx5_public nvmeib_common_mlx4_public nvmeib_common_public nvmeib_common
		#(cd ~/projects/ssda; sudo dpkg -i --force-overwrite ./nvmesh-core_*.deb) || return 1;
		#(cd ~/projects/ssda; sudo yum install -y -C `ls -t nvmesh-core-*.rpm | head -1`) || return 1;
		(cd ~/projects/ssda; sudo sudo rpm -ivh --nodeps `ls -t nvmesh-target-*.rpm | head -1`) || return 1;
		sudo touch /var/opt/nvmesh/.target_devices;
		#sudo chkconfig nvmeshtarget off;
		#sudo dh_systemd_enable --no-enable nvmeshtarget;
		#sudo systemctl disable --global nvmeshtarget;
		sudo systemctl stop nvmeshtarget; sudo systemctl disable nvmeshtarget;
		#sudo chkconfig nvmeshclient off;
		#sudo dh_systemd_enable --no-enable nvmeshclient;
		#sudo systemctl disable --global nvmeshclient;
		sudo systemctl stop nvmeshclient; sudo systemctl disable nvmeshclient;
		sudo cp ~/nvmesh.conf.Ronen /etc/nvmesh/nvmesh.conf || return 1;
		sudo cp ~/nvmesh_options_pcpu_nrch.conf.Ronen /etc/modprobe.d/nvmesh_options_pcpu_nrch.conf || return 1;
#		#deb_file=`ssh nvme1013 ls -tr /data/management-rpms/management_master_HEAD/utils/nvmesh-utils\*amd64.deb | tail -1`;
#		rpm_file=`ssh nvme1013 ls -tr /data/management-rpms/management_master_HEAD/utils/nvmesh-utils\*x86_64.rpm | grep -v ubuntu | tail -1`;
#		rpm_file=`ssh nvme1013 ls -tr /data/management-rpms/management_master_*/utils/nvmesh-utils\*x86_64.rpm | grep -v ubuntu | tail -1`;
#		#rm nvmesh-utils*amd64.deb;
#		rm nvmesh-utils*x86_64.rpm;
#		#scp nvme1013:${deb_file} .
#		scp nvme1013:${rpm_file} .
#		#sudo dpkg -i --force-overwrite ./nvmesh-utils*.deb || return 1;
#		sudo yum install -y ./nvmesh-utils*.rpm || return 1;
		#sudo service nvmeshclient restart;
		#sudo dh_systemd_start nvmeshclient;
		sudo systemctl restart nvmeshclient;
		#sudo service nvmeshtarget restart;
		#sudo dh_systemd_start nvmeshtarget;
		sudo systemctl restart nvmeshtarget;
		_rv_=0;
		while (($_rv_==0)); do
			sudo bash -c "echo 1 > /sys/module/nvmeibs/parameters/debug_level" && _rv_=1 || sleep 1;
		done;
		_rv_=0;
		while (($_rv_==0)); do
			sudo bash -c "echo 1 > /sys/module/nvmeibc/parameters/debug_level" && _rv_=1 || sleep 1;
		done;
		_rv_=0;
		_iter_=0;
		while (($_rv_==0 && $_iter_<5)); do
			pgrep nvmeibt_toma && _rv_=1 || sleep 1;
			_iter_=$(($_iter_+1));
		done;
		_rv_=0;
		while (($_rv_==0)); do
			sudo pkill -9 nvmeibt_toma;
			pgrep nvmeibt_toma && sleep 1 || _rv_=1;
		done;

		sudo cp -p /tmp/backup/toma_persistence_*_raft_and_topo.0 /var/opt/nvmesh/toma/
		sleep 2;
		echo "Finished run_full_init"
        };  __run_full_init'

	# Setup machines
	alias open_firewall_other='sudo iptables -I INPUT 1 -i -p tcp --dport 4000 -j ACCEPT; sudo iptables -I INPUT 1 -i -p tcp --dport 4001 -j ACCEPT'
	alias setup_excelero_node='sudo bash -c '"'"'echo MANAGEMENT_SERVER="http://10.0.1.42:4000" > /etc/nvmesh/nvmesh.conf'"'"'; sudo mkdir /var/opt/nvmesh/toma; sudo mkdir /var/opt/nvmesh/bin; sudo mkdir /var/opt/nvmesh/toma/; sudo bash -c '"'"'echo "+ all" > /var/log/nvmesh/trace.config'"'"
	alias stop_nvmesh_services='sudo /sbin/chkconfig nvmeshtarget1 off; sudo service nvmeshtarget stop; sudo /sbin/chkconfig nvmeshclient off; sudo service nvmeshclient stop'
	alias client_debug='sudo bash -c "echo 2 > /sys/module/nvmeibc/parameters/debug_level"'

	alias fix_mlx_udev='sudo echo KERNEL==i\"uat\", SYMLINK+=\"infiniband/%k\", MODE=\"0666\" >> /etc/udev/rules.d/90-ib.rules ; sudo udevadm control --reload'
	alias hard_reboot='sudo bash -c "echo 1 > /proc/sys/kernel/sysrq; echo b > /proc/sysrq-trigger"'
	alias format_the_disk_drives_of_nvme4x_no_EC='
		rmmod nvmeibs;
		rmmod nvme;
		modprobe nvme;
		nvme id-ns /dev/nvme0n1;
		# Use option 3 in the format
		nvme format /dev/nvme0n1 -l 3;           # repeat twice
		nvme format /dev/nvme0n1 -l 3;           # repeat twice
		rmmod nvme;
		modprobe nvme;
		nvme list;
	'
	alias my_rmmod='sudo rmmod `lsmod | grep nvme | cut -f1 -d" "`; lsmod | grep nvme'
	alias my_insmod='sudo insmod ~/projects/ssda/common/nvmeib_common.ko; sudo insmod ~/projects/ssda/symvers/symvers.ko; sudo insmod ~/projects/ssda/common_public/nvmeib_common_public.ko; sudo insmod ~/projects/ssda/common_public/nvmeib_common_mlx5_public.ko; sudo insmod ~/projects/ssda/srv/nvmeibs.ko; sudo ~/projects/ssda/management_cm/managementCM.py start; sudo ~/projects/ssda/tools/trace_daemon_2.0/trace_daemon &'
	alias blacklist_exclude_PCI_disk_drive_rule_file='echo /etc/udev/rules.d/59-nvme-drives-a.rules'
	#################################################################################################################################
else	###########################################              my-laptop) code             ############################################
	#################################################################################################################################

	# From Ofer
	alias clean_nvmesh='
		(cd ~/projects/ssda;
		find . -type f -name "*.i" -delete;
		find . -type f -name "*.o" -delete;
		find . -type f -name "*.o.*" -delete;
		find . -type f -name "*.d" -delete;
		find . -type f -name "*.so" -delete;
		find . -type f -name "dict.*.json" -delete;
		find . -type f -name "*.trace.json" -delete;
		find . -type f -name "*.trace.json.*" -delete;
		find . -type f -name "gen_events.h" -delete;
		find . -name .trace_pp_dir -type d -exec rm -rf {} +;
		rm -rf symvers;
		rm -f Module.symvers;
		rm -rf toma/trace;
		rm -rf autogen/*
		rm management_cm/mcs_messages_viewer.pyc
		rm management_cm/send_to_mcs.pyc
		rm -rf toma/i/* toma/obj/*
		)
	'
	alias make_toma_local_clang_release="make -j32 all LLVM=true CC=clang LD=ld.lld MOD=release NVMEIBC_SECTOR_SHIFT=12 AUTOGEN_DIR=/home/${USER}/projects/ssda/autogen AUTOGEN_SUBDIRS_TOMA='common toma'"
	alias make_toma_local_clang_debug="make -j32 all LLVM=true CC=clang LD=ld.lld NVMEIBC_SECTOR_SHIFT=12 AUTOGEN_DIR=/home/${USER}/projects/ssda/autogen AUTOGEN_SUBDIRS_TOMA='common toma'"

	gsettings set org.gnome.desktop.wm.keybindings switch-input-source-backward "['<Alt>Shift_R']"	# Switch language
	if [ ! -e ~/bin/switch_env_to_git_branch.sh ] ; then ln -s ~/projects/ssda/switch_env_to_git_branch.sh ~/bin/switch_env_to_git_branch.sh; fi;	source /usr/share/bash-completion/completions/git; 	__git_complete switch_env_to_git_branch.sh _git_branch; __git_complete ./switch_env_to_git_branch.sh _git_branch; __git_complete ../switch_env_to_git_branch.sh _git_branch	## autocomplete using git branches
	alias recover_from_anyconnect='sudo systemctl restart systemd-resolved.service'
	alias UNUSED-anyconnect_save='sudo iptables-save -f ~/iptables_save.txt'
	alias UNUSED-anyconnect_restore='sudo iptables-restore -f ~/iptables_save.txt'
	alias add_deep_sleep='sudo grubby --update-kernel=ALL --args="mem_sleep_default=deep"'
	alias grep_alloc_free="grep -rEn '\<kfree\>|\<vfree\>|\<kalloc\>|\<kzalloc\>|\<kcalloc\>|\<krealloc\>|\<vzalloc\>|\<vmalloc\>|\<calloc\>|\<kmem_alloc\>|\<malloc\>' clnt"
	alias ping_nvme1006_IPMI='ping nvme1006.ilo'	# nvme1006-ilo/ ADMIN ADMIN	# Enter remote control iKVM/HTML5
	alias resolveall='resolvectl query www.google.com geovpn.mellanox.com confluence.nvidia.com nvme1014.lab.nvidia.com nvme1014';
	#LOGS & stat
	alias print_toma_simulator_cmds='
		cd toma/unitest/;
		echo "make clean;    make all -j;   rm _root/var/log/nvmesh/trace_daemon/*binlog*;	./nvmeibt_toma";
		echo "./_root/var/log/nvmesh/trace_daemon/pager _root/var/log/nvmesh/trace_daemon --toma --color";
		echo "If you dont have pager on your laptop run\n	./build-verify.sh";
	'
	alias compile_toma_simulator="
		rsync --ignore-errors --delete --delete-before --exclude-from=${HOME}/projects/ssda/excludes -rlpgoDv --sparse ~/projects/ssda/ ~/projects/ssda_tmp;
		cd ~/projects/ssda_tmp/toma/unitest;
		make clean;    make all -j;
	"
	alias my_compile_block_simulator="
		#rsync --ignore-errors --delete --delete-before --delete-excluded --exclude-from=~/projects/ssda/excludes -rlpgoDv ~/projects/ssda/ ~/projects/ssda_tmp;
		rsync --ignore-errors --delete --delete-before --exclude-from=/home/alexander/projects/ssda/excludes -rlpgoDv --sparse ~/projects/ssda/ ~/projects/ssda_tmp;
		cd ~/projects/ssda_tmp/clnt/block/unitest;
		cp ~/launch.json ./.vscode/launch.json
		./build_block_testing.sh build;
	"
#	alias my_run_simulator='sudo rm /var/lib/systemd/coredump/core* ./core; ./run_block_unitest.sh -n -d f -nRep 2 -hsync; sudo unzstd -o ./core /var/lib/systemd/coredump/core*zst; echo "Running pager"; ./pager -t now-80s > log.txt'
	function my_run_block_simulator {
		sudo rm -f /var/lib/systemd/coredump/core* ./core;
		if [ "_$1" == "_" ] ; then
			n=1;
		else
			n=$1;
		fi
		rv=0;
		iter=1;
		while (( ${iter} <= ${n} && ${rv} == 0 )) ; do
			time ./run_block_unitest.sh -n -d f -nRep 2 -hsync;
			rv=$?
			if (( "${rv}" == "0" )) ; then
				echo "=========== SUCESS iteration ${iter}/${n} ===========";
				echo "=========== SUCESS iteration ${iter}/${n} ===========" > SUCCESS_iteration_counter;
				sleep 2
			else
				echo "----------- FAILURE iteration ${iter}/${n} -----------";
				sudo time unzstd -o ./core /var/lib/systemd/coredump/core*zst;
				sudo chown ${USER}:${USER} core
			fi
			((iter++));
		done
		echo "Running pager";
		time ./pager -t now-80s > log.txt;
	}


	alias backup_simulator_run='cp -rp ../../.. ~/LOGS/unitests/simulator_`date +_%d%b%Y_%H%M%S`'
	alias copy_bashrc='
		for i in ${my_servers} ${my_other_servers} ${my_servers_2}; do
			host="${i}";
			echo ${host};
			scp ~/.bashrc ${host}:
			scp ~/nvmesh.conf.Ronen ${host}:nvmesh.conf.Ronen---nvme1014
		done;
	'
	function __copy_and_open_logs_from_host() {
		host=$1;
		which=$2;
		mmin=$3;
		username=$4;
		n_prev_runs=1
		# For now, until we can identify the boundaries between runs
		if [ "_${which}" == "_toma" ] || [ "_${which}" == "_toma_nomarker" ]; then
			binlog_prefix='toma'
		elif [ "_${which}" == "_toma_util" ]; then
			binlog_prefix='toma_util'
		elif [ "_${which}" == "_clnt" ]; then
			binlog_prefix='nvmeibc_trace'
		elif [ "_${which}" == "_srv" ]; then
			binlog_prefix='nvmeibs_trace'
		elif [ "_${which}" == "_common" ]; then
			binlog_prefix='nvmeibm_trace'
		fi
		if [ "_${which}" == "_toma" ] || [ "_${which}" == "_toma_nomarker" ]; then
			ssh ${username}@${host} sudo pkill -10 nvmeibt_toma;
			ssh ${username}@${host} "sudo nohup cat /proc/nvmeibs/toma_status/all" > ${host}_stat.all &
			marker_file=`ssh ${username}@${host} "ls -tr /var/log/nvmesh/trace_daemon/toma.binlog_marker* | tail -${n_prev_runs} | head -1"`
		else
			marker_file=''
		fi
		if [ "_${which}" == "_toma_nomarker" ]; then
			marker_file='';
			which='toma';
		fi
		local_binlogs_dir=binlogs_${host}_${which};
		rm -rf ${local_binlogs_dir}/*binlog*;
		mkdir -p ${local_binlogs_dir};
		if [ "_${marker_file}" == "_" ]; then
			last_binlog_files=`ssh ${username}@${host} "find /var/log/nvmesh/trace_daemon/ -name ${binlog_prefix}\* -mmin -${mmin}"`
		else
			last_binlog_files=`ssh ${username}@${host} "find /var/log/nvmesh/trace_daemon/ -name ${binlog_prefix}\* -newer ${marker_file}"`
		fi
		one_liner=`echo ${last_binlog_files} ${marker_file} | sed -e 's/ / \:/g'`
		# echo "***************** ${one_liner} ******************"
		if [ "_${one_liner}" != "_" ]; then
			rsync --delete --delete-before -avP -z --checksum --sparse ${username}@${host}:${one_liner} ${local_binlogs_dir} || return 1;
		fi
		rsync --delete --delete-before -avP -z --checksum --sparse ${username}@${host}:"/var/log/nvmesh/trace_daemon/dict.*" ${local_binlogs_dir} || return 1;
		rsync -avP -z --checksum --sparse --copy-links --links ${username}@${host}:"/var/log/nvmesh/trace_daemon/*pager*" ${local_binlogs_dir} || return 1;
		(cd ${local_binlogs_dir}; ./pager.py --${which}) > ${host}_${which}.log;
		if [ "_${which}" == "_toma" ]; then
			rsync --delete --delete-before -avP -z --checksum --sparse ${username}@${host}:/var/opt/nvmesh/toma ${host}_opt_NVMesh
			ssh "${host}" "nohup sleep 5; sudo ls -tr /var/log/nvmesh/*.stat | tail -1 | xargs nohup cat" > "${host}".stat &
		fi
	};
	function __copylogs() {
		which=$1; shift;
		mmin=$1; shift;
		is_cleanup=$1; shift;
		username=$1; shift
		if [ "_${username}" == "_" ]; then
			username=$USERNAME;
		fi
		rm *.stat;
		for i in $@; do
			host="${i}"
			__copy_and_open_logs_from_host ${host} ${which} ${mmin} ${username} &
			if [ "_${is_cleanup}" == "_1" ]; then
				ssh ${username}@${host} 'sudo find /var/log/nvmesh/trace_daemon -name nvmeib\* toma\* -mtime +10 -delete'
			fi
		done
	};
	function __copylogs_everything() {
		is_cleanup=$1; shift;
		username=$1; shift
		if [ "_${username}" == "_" ]; then
			username=$USERNAME;
		fi
		rm *.stat;
		for i in $@; do
			host="${i}"
			__copy_and_open_logs_from_host ${host} 'toma_nomarker' 60000 ${username} &
			__copy_and_open_logs_from_host ${host} 'clnt' 60000 ${username} &
			__copy_and_open_logs_from_host ${host} 'srv' 60000 ${username} &
			__copy_and_open_logs_from_host ${host} 'common' 60000 ${username} &
			nohup rsync --delete --delete-before -avP -z --checksum --sparse ${username}@${host}:"/var/lib/kafka/" ${host}_kafka &
			nohup ssh ${username}@${host} "sudo journalctl --output=short-precise --since=-100000" > ${host}.ctl &
			if [ "_${is_cleanup}" == "_1" ]; then
				ssh ${username}@${host} 'sudo find /var/log/nvmesh/trace_daemon -name nvmeib\* toma\* -mtime +10 -delete'
			fi
		done
	};
	function __copylogs_10_hours() {
		is_cleanup=$1; shift;
		username=$1; shift
		if [ "_${username}" == "_" ]; then
			username=$USERNAME;
		fi
		rm *.stat;
		for i in $@; do
			host="${i}"
			__copy_and_open_logs_from_host ${host} 'toma_nomarker' 600 $username &
			__copy_and_open_logs_from_host ${host} 'clnt' 600 $username &
			__copy_and_open_logs_from_host ${host} 'srv' 600 $username &
			__copy_and_open_logs_from_host ${host} 'common' 600 $username &
			nohup rsync --delete --delete-before -avP -z --checksum --sparse ${username}@${host}:"/var/lib/kafka/" ${host}_kafka &
			nohup ssh ${username}@${host} "sudo journalctl --output=short-precise --since=-1000" > ${host}.ctl &
			if [ "_${is_cleanup}" == "_1" ]; then
				ssh ${username}@${host} 'sudo find /var/log/nvmesh/trace_daemon -name nvmeib\* toma\* -mtime +10 -delete'
			fi
		done
	};
	alias copylogs_toma_username='__copylogs toma 6000 0 $@';
	alias copylogs_toma='__copylogs toma 60 0 $USERNAME $@';
	alias copylogs_toma_util='__copylogs toma_util 60 0 $USERNAME $@';
	alias copylogs_toma_full='__copylogs toma 6000 0 $USERNAME $@';
	alias copylogs_everything='__copylogs_everything 0 $USERNAME $@'

	alias myfor='function __execute_on_my_server() {
		for i in ${my_servers}; do
			host="${i}"
			echo ${host};
			ssh ${host} $@;
		done
	};  __execute_on_my_server'
	alias myfor_stop_nvmesh_services='
		for i in ${my_servers}; do
                        host="${i}"
                        echo ${host};
			rv=1
			(
                        while [ $rv != 0 ] ; do
                                ssh ${host} "sudo systemctl disable nvmeshtarget; sudo systemctl stop nvmeshtarget; sudo systemctl disable nvmeshclient; sudo systemctl stop nvmeshclient;";
				rv=$?;
                        done
			) &
                done
	'
	alias diff_persist_files="for i in {0..8}; do let j=(\$i+1); echo \$i \$j; diff -s -q /var/opt/excelero/toma/toma_persistency.csv_bufs.[\$i,\$j]; done"

	# git
	alias gitConfigEdit="git config --global --edit"
	# Paolo's http://www.linux.com/news/featured-blogs/200-libby-clark/821899-git-success-stories-and-tips-from-kvm-maintainer-paolo-bonzini
	alias gitchanges="git diff --name-status -r"
	alias gitdiffstat="git diff --stat -r"
	alias gitwhatis="git show -s --pretty='tformat:%h (%s, %ad)' --date=short"
	alias gitpwhatis="git show -s --pretty='tformat:%h, %s, %ad' --date=short"
	alias git_show_origins="git branch -vv"
	alias git_set_remote="git branch --set-upstream-to=origin/xxxxxxxxxxxxxxxxxxx"
	alias git_branch_dates='function __git_branch_dates() { git for-each-ref --sort='\''-committerdate'\'' --format='\'' C=%(committerdate:rfc2822)   %(refname:short)'\'' refs/"$1" refs/remotes/"$1" | head -10; }; __git_branch_dates'
	alias git_branch_dates_all='function __git_branch_dates_all() { git for-each-ref --sort='\''-committerdate'\'' --format='\'' C=%(committerdate:rfc2822)   %(refname:short)'\'' refs/"$1" refs/remotes/"$1"; }; __git_branch_dates_all'
	alias git_push_any_to_new='git push -f ronen 70e4c44569ca8706fxxxxxxxxxxxxxxxxxxxxx:refs/heads/CI_master_Oct10_1'
	alias git_del_old_CI_branches='function __git_del_old_CI_branches() {
		__del_list=`git for-each-ref --sort="-committerdate:rfc2822" --format="%(refname:lstrip=3)" refs/remotes/ronen/#[Cc][Ii]* | tail -n +20` || return 1;
		for i in ${__del_list} ; do
			echo --${i}-- ;
			git push ronen :${i} ;
		done ;
	} ; __git_del_old_CI_branches'
	alias git_log_lines_range='git log -L "2104,2109:nvmeibt_praid.c"'
	alias git_show_stash='git show stash@{0}'
	alias grep_git_conflicts="grep -Ern '^<<<<<<< |^\=\=\=\=\=\=\=$|^>>>>>>> |^\|\|\|\|\|\|\|' ~/projects/ssda/"

	# Stuck things
	alias restartwifi="sudo rmmod iwlmvm; sudo insmod /usr/lib/modules/`uname -r`/kernel/drivers/net/wireless/iwlwifi/mvm/iwlmvm.ko.xz"
	alias restart_files='killall -r gvfs\*'

	# Run misc
	alias backup_ssda='backup_dir=~/projects/backups/ssda`date +_%d%b%Y_%H%M%S` ; cp -rp ~/projects/ssda ${backup_dir} ; touch ${backup_dir}'
	alias compare_backup_ssda='backup_dir=`ls -trd ~/projects/backups/ssda* | tail -1` ; for i in `find . -name \*\.\[ch\]` ; do echo === $i ===; diff $i ${backup_dir}/toma/$i ; done'
	alias track_resources="watch '(for i in ${my_servers}; do echo \$i; ssh \$i "'"'"ps -lye | grep nvmeibt_toma"'"'"; done)'"
	alias rm_dict_files='myfor "sudo rm -rf projects/ssda/*/trace/*/*/dict\.[0-9]*\.json /var/log/nvmesh/*; sudo mkdir /var/log/nvmesh/trace_daemon"'
	alias make_clean='
		pushd ~/projects/ssda;
		for i in ${my_other_servers} ${my_servers_2} ${my_servers}; do
			echo "**** ${i} ****";
			rsync --ignore-errors --delete --delete-before --delete-excluded --exclude-from=excludes -rlpgoDv -z --checksum --sparse . -e ssh ${i}:projects/ssda&
		done;
		popd;
		rm_dict_files;
	'
	# mgt
	alias remote_access_mgt='sh -f -N -L 24000:10.0.1.43:4000 ronen@81.218.148.131' # browse to https://localhost:24000
	alias reinstall_mgt='
		(
                mgmt_server_name=${my_mgmt_server};
                read -p "mgmt_server_name? (Default ${mgmt_server_name})? " server_name;
                if [ "_${server_name}" != "_" ]; then
                        mgmt_server_name=${server_name};
                fi;
                servers_list="nvme1014,nvme1015,nvme1018";
                read -p "servers_list? (Default ${servers_list})? " all_servers_list;
                if [ "_${all_servers_list}" != "_" ]; then
                        servers_list=${all_server_list};
                fi;
		cd ~/projects/management;
		branch_name=`git rev-parse --abbrev-ref HEAD`;
                read -p "branch_name? (Default ${branch_name})? " br_name;
                if [ "_${br_name}" != "_" ]; then
                        branch_name=${br_name};
                fi;
		# git fetch -p --all;
		git fetch -p;
		git checkout ${branch_name};
		git rebase;
		cd ~/projects/infra;
		git fetch -p;
		git checkout -b master upstream/master
		git rebase;
		echo ~/projects/infra/xlro/infra/bin/install.sh -w -m ~/projects/management -n ~/projects/ssda ${mgmt_server_name} ${servers_list};
		~/projects/infra/xlro/infra/bin/install.sh -w -m ~/projects/management -n ~/projects/ssda ${mgmt_server_name} ${servers_list};
		)
	:'
	#
	######## resolve.conf ########
	cat << "	END" > /tmp/resolv.conf_OFFICE_before_AUG22_2023
	search mellanox.com nvidia.com mtl.com labs.mlnx mtl.labs.mlnx mth.labs.mlnx mtr.labs.mlnx mtl.labs.mlnx lab.mtl.com mts.labs.mlnx wap.labs.mlnx swx.labs.mlnx yok.mtl.com
	# Needed? search ezchip.com
	# nameserver 10.0.16.151
	# nameserver 10.0.16.152
	options edns0 trust-ad
	nameserver 127.0.0.53
	nameserver 10.0.16.151
	nameserver 10.0.16.152
	END

	cat << "	END" > /tmp/resolv.conf_HOME_BEFORE_RUNNING_PULSECURE
	search mellanox.com nvidia.com mtl.com labs.mlnx mtl.labs.mlnx mth.labs.mlnx mtr.labs.mlnx mtl.labs.mlnx lab.mtl.com mts.labs.mlnx wap.labs.mlnx swx.labs.mlnx yok.mtl.com
	# Needed? search ezchip.com
	# nameserver 10.0.16.151
	# nameserver 10.0.16.152
	options edns0 trust-ad
	nameserver 127.0.0.53
	END

	cp /tmp/resolv.conf_HOME_BEFORE_RUNNING_PULSECURE /tmp/resolv.conf_OFFICE

	cat << "	END" > /tmp/resolv.conf_HOME_EDITED_BY_PULSE_before_Aug24_2023
	search mellanox.com nvidia.com mtl.com labs.mlnx mtl.labs.mlnx mth.labs.mlnx mtr.labs.mlnx mtl.labs.mlnx lab.mtl.com mts.labs.mlnx wap.labs.mlnx swx.labs.mlnx yok.mtl.com excelero.com
	nameserver 10.0.16.151
	nameserver 10.0.16.152
	nameserver 192.168.0.1
	END

	cat << "	END" > /tmp/resolv.conf_HOME_BY_PULSE_Aug24_2023_Working
	search excelero.com nvidia.com labs.mlnx mtl.labs.mlnx mth.labs.mlnx mtr.labs.mlnx mtl.labs.mlnx mts.labs.mlnx wap.labs.mlnx swx.labs.mlnx yok.mtl.com mtl.com lab.mtl.com mellanox.com
	nameserver 192.168.0.1
	nameserver 10.0.15.56
	nameserver 10.0.8.3
	END

	alias resolv.conf_fix='
		home_ip_prefix="192\.168\.";
		sudo rm -f /etc/resolv.conf	# If we have a symbolic link, be sure not to affect that file
		ifconfig | grep -E " inet " | grep -E "${home_ip_prefix}";
		if (( $? == 1 )) then
			echo "--- OFFICE ---"
			sudo cp /tmp/resolv.conf_OFFICE /etc/resolv.conf;
		else
			sudo systemctl start systemd-resolved.service; sudo systemctl start pulsesecure.service;
			pgrep pulseUI
			if (( $? == 1 )) then
				echo "--- HOME_BEFORE_RUNNING_PULSECURE ---"
				sudo cp /tmp/resolv.conf_HOME_BEFORE_RUNNING_PULSECURE /etc/resolv.conf;
			else
				echo "--- HOME_EDITED_BY_PULSE ---"
				sudo cp /tmp/resolv.conf_HOME_BY_PULSE_Aug24_2023_Working /etc/resolv.conf;
			fi
		fi
		resolv_links=`resolvectl domain | tail -n +2 | cut -d" " -f2`;
		for i in ${resolv_links}; do sudo resolvectl domain ${i} search mtl.labs.mlnx; done;
		resolvectl query www.google.com geovpn.mellanox.com confluence.nvidia.com nvme1014.lab.nvidia.com nvme1014;
	'
	alias generate_hosts_file='
		(
			for i in ${MTV_EXCELERO1} nvmeserver2 gitlab-mirror-mtl.nvidia.com gitlab-master.nvidia.com confluence.nvidia.com; do n=${i}; resolvectl query "${n}" | grep ${n}: | awk '"'"'{print $2 "	" $1}'"'"' | sed "s/://" ; done
			for i in {0..2000}; do n=nvme${i}; resolvectl query "${n}" | grep ${n}: | awk '"'"'{print $2 "	" $1 "  	" $1".lab.nvidia.com"}'"'"' | sed "s/://g" ; done
		) > ~/.ssh/auto_generated_hosts_file
	'
	alias kernel_vmcore_debugging_rocky='
		// Omri Levi wrote:  I google the uname -r result then look in the debug/ folder
		dnf install crash
		wget https://dl.rockylinux.org/vault/rocky/8.6/BaseOS/x86_64/debug/tree/Packages/k/kernel-debuginfo-4.18.0-372.19.1.el8_6.x86_64.rpm
		wget https://downloads.rockylinux.org/vault/rocky/8.6/BaseOS/x86_64/debug/tree/Packages/k/kernel-debuginfo-common-x86_64-4.18.0-372.19.1.el8_6.x86_64.rpm
		dnf install kernel-debuginfo-common-x86_64-4.18.0-372.19.1.el8_6.x86_64.rpm
		dnf install kernel-debuginfo-4.18.0-372.19.1.el8_6.x86_64.rpm
		crash /usr/lib/debug/lib/modules/4.18.0-372.19.1.el8_6.x86_64/vmlinux vmcore
	'
fi

#################################################################################################################################
###########################################   COMMON (both remote and local)    #############################################
#################################################################################################################################

function open_logs_collecteor_all_hosts {
    for i in *_nvmesh_logs_*.tgz; do
	    host=`echo ${i} | cut -d"_" -f 1 -`;
	    tar --no-same-owner -xvpSf $i;
	    logdir=`echo ${host}*/var/log/nvmesh/trace_daemon`; if [[ "_${logdir}" == "_" ]]; then logdir=`echo ${host}*/var/log/NVMesh/trace_daemon`; fi
	    echo "======= host=${host}  logdir=${logdir} ======="=
	    (cd ${logdir}; ./pager.py --toma > ../../../../../${host}_toma.log) &
	    (cd ${logdir}; ./pager.py --clnt > ../../../../../${host}_clnt.log) &
	    #
	    pgp_file=`echo ${host}_nvmesh_*/var/log/nvmesh/nvmeibt_toma_src_tar.pgp`
	    toma_src_dir_fr_pgp="${host}_toma_src_dir"
	    mkdir ${toma_src_dir_fr_pgp}
	    (cd ${toma_src_dir_fr_pgp}; gpg -d -z6 --batch --pinentry-mode default --passphrase AlexanderRonen < ../${pgp_file} | tar -xvpf -)
	    #
	    (cd ${toma_src_dir_fr_pgp}; tar -xvpf ../${host}_nvmesh_*/toma_libs_*.tar)	# Open the /lib64 of the machine
    done;
}

alias copybug='function __copybug() {
	bugs_host="${MTV_EXCELERO1}"
	bugs_dir="/auto/nvmesh_log/logs"
	shopt -s nullglob;
	#bugdir=`ssh ${bugs_host} "cd /home/qa/logs; ls -d *${1}*"` || return 1;
	bugdir=`ssh ${bugs_host} "cd ${bugs_dir}; ls -d *${1}*"` || return 1;
	echo --------- $bugdir --------;
	ssh ${bugs_host} "sudo chmod -R a+r ${bugs_dir}/${bugdir}";
	rsync -avP -zz --sparse --exclude="\*.csv" ${bugs_host}:${bugs_dir}"/${bugdir}" ~/LOGS;
	cd ~/LOGS/"${bugdir}" || return 1;
	open_logs_collecteor_all_hosts;
}; __copybug $@'

alias copyCI='function __copyCI() {
	bugs_host="${MTV_EXCELERO1}"
	jenkins_log_dir="/logs/jenkins"
	shopt -s nullglob;
	dir_search_name=${1};
	dir_search_name_len=${#dir_search_name};
	if (( ${dir_search_name_len} < 6 )); then
		dir_search_name="jenkins-*-${dir_search_name}";
	fi
	bugdir=`ssh ${bugs_host} "cd ${jenkins_log_dir}; ls -d *${dir_search_name}*"` || return 1;
	echo --------- $bugdir --------;
	ssh ${bugs_host} "sudo chmod -R a+xr ${jenkins_log_dir}/${bugdir} ${jenkins_log_dir}/${bugdir}/*";
	rsync -avP -zz --copy-links --sparse --exclude="\*.csv" ${bugs_host}:${jenkins_log_dir}/${bugdir} ~/LOGS; cd ~/LOGS/${bugdir}/ || return 1;
	for j in ~/LOGS/${bugdir}/*; do
		cd ${j}
		open_logs_collecteor_all_hosts;
		(mkdir infra_logs; cd infra_logs; tar -xvpSf ../infra_logs.tar.gz);
	done;
}; __copyCI'

# Uncomment the following line if you don't like systemctl's auto-paging feature:
# export SYSTEMD_PAGER=
