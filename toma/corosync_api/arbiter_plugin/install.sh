# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

#####################################  Compile plugin into corosync library to be an arbiter.
# Run executable on each node of the cluster, or insert the code into Toma so toma is the executable!

####### Install corosync
	# 1. sources (for compiling arbiter
	# 2. rpm for running coro_sync in case that pace maker with coro_sync does not exist
[[ ! -z "`cat /etc/os-release | grep NAME | grep Ubuntu`" ]] && IS_UBUNTU="Y" || IS_UBUNTU="N";

cur_dir=${PWD};
CORO_SRC="/home/${USER}/projects/corosync";
if [ ! -d ${CORO_SRC} ]; then
	if [[ $IS_UBUNTU == "Y" ]]; then	# For Debian/Ubuntu
		sudo apt install libknet1 libknet-dev libnss3-dev libnet1-dev groff;  # Libraries to compile arbiter
		# sudo apt install libnspr4-dev
		# Todo: rpm for corosync only. Installing pace maker below brings corosync but this is an overkill
		# sudo apt install pacemaker pcs resource-agents pacemaker-dev crmsh;
	else								# Centos
		# Todo:
		sudo yum install pacemaker-devel;
		sudo rpm -q --queryformat '%{BUILDFLAGS}\n' pacemaker;
	fi

	cd ~/projects/;
	git clone https://github.com/corosync/corosync.git
	cd corosync;
	./autogen.sh;
	./configure;
	make;

	if false; then	# Install qdevice plugin into corosync lib - not needed, we work directly with corosync api
		git clone https://github.com/corosync/corosync-qdevice.git
	fi
	cd ${cur_dir};
else
	echo "Coro_sync repo already exists";
fi

####### Check corosync version
if true; then
	/usr/sbin/corosync -v;
	sudo find /usr -name "corosync";
fi

####### Run test environment of corosync, do in different terminal
if false; then
	# Stop corosync: optional
	COR_LOG_FILE="/var/log/corosync/corosync.log";
	ps -ef | grep coro;
	sudo systemctl stop corosync;
	sudo rm -f ${COR_LOG_FILE}

	# Prepare unitest test cluster
	sudo cp ./corosync_cluster.conf /etc/corosync/corosync.conf;

	# Run corosync in foreground debug mode
	clear; sudo /usr/sbin/corosync -f;
	# Monitor: sudo journalctl -u corosync; less ${COR_LOG_FILE};
fi

####### Compile and run the arbiter
if true; then
	echo "Compile the arbiter";
	EXE_NAME="arbiter";

	# clean all
	cmd="rm -f ${EXE_NAME}.[od] ${EXE_NAME}; rm -rf ./.libs";
	echo -e "\n_______\n${cmd}"; eval ${cmd};

	FLAGS="-g -O2  -fPIC -DPIC  -fPIE -O3 -ggdb3 -Wall -Wshadow -Wmissing-prototypes -Wmissing-declarations -Wstrict-prototypes -Wpointer-arith -Wwrite-strings -Wcast-align -Wbad-function-cast -Wmissing-format-attribute -Wformat=2 -Wformat-security -Wformat-nonliteral -Wno-long-long -Wno-strict-aliasing";
	cmd="gcc -I${CORO_SRC}/include/corosync -I${CORO_SRC}/include ${FLAGS} -pthread -MT ${EXE_NAME}.o -MD -MP -c -o ${EXE_NAME}.o ${EXE_NAME}.c";
	echo -e "\n_______\n${cmd}"; eval ${cmd};
	cmd="${CORO_SRC}/libtool --silent --tag=CC --mode=link gcc ${FLAGS} -pthread -fPIC -DPIC -pie -Wl,-z,relro -Wl,-z,now  -Wl,--as-needed -o ${EXE_NAME} ${EXE_NAME}.o ${CORO_SRC}/lib/libvotequorum.la -lqb -lrt -lpthread";
	echo -e "\n_______\n${cmd}"; eval ${cmd};

	# Cleanup intermediate build results
	rm ${EXE_NAME};
	cp .libs/${EXE_NAME} ${EXE_NAME};
	rm -rf ./.libs;

	echo -e "\n_______ run_exe\n";
	sudo ./${EXE_NAME};
fi
