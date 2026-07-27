#####################################  Install alternative to corosync library to use in pace maker.
# Run me on each node of the cluster!

####### Install pace maker to test the library
[[ ! -z "`cat /etc/os-release | grep NAME | grep Ubuntu`" ]] && IS_UBUNTU="Y" || IS_UBUNTU="N";
if false; then
	if [[ $IS_UBUNTU == "Y" ]]; then	# For Debian/Ubuntu
		sudo apt install pacemaker pcs resource-agents pacemaker-dev;
		sudo apt install crmsh;
	else
		sudo yum install pacemaker-devel;
		sudo rpm -q --queryformat '%{BUILDFLAGS}\n' pacemaker;
	fi
	sudo passwd hacluster;	# Daniel created password 12345
fi
pacemakerd --version;
pacemakerd -F; # | grep custom

####### Install library
echo "Building the library";
make all;
LIB_DIR=/usr/local/lib/;
LIB_NAME="libclusterstack";
sudo rm -f ${LIB_DIR}${LIB_NAME}.so;
cmd="cp ${LIB_NAME}.so ${LIB_DIR}"; echo ${cmd}; eval "sudo ${cmd}";
sudo ls -l "${LIB_DIR}${LIB_NAME}.so";
sudo ldconfig;

if false; then		# Create pace-maker General configuration
	sudo mkdir -p /etc/pacemaker;
	nvmeibc_conf="/etc/pacemaker/pacemaker.conf";
	nvmeibc_conf_marker='# Toma-based corosync';
	sudo rm -f ${nvmeibc_conf};
	sudo tee ${nvmeibc_conf} << EOF
# Toma-based corosync
cluster_stack = "custom"
custom_stack_lib = "${LIB_DIR}${LIB_NAME}.so"
##################
EOF
	sudo ls -l ${nvmeibc_conf};
fi

# Create systemd override for Pacemaker. Can do the below manually via 'sudo systemctl edit pacemaker'
sudo mkdir -p /etc/systemd/system/pacemaker.service.d/
sudo tee /etc/systemd/system/pacemaker.service.d/override.conf << EOF
[Service]
#Environment="PCMK_cluster_type=custom"
Environment="PCMK_stack_file=/usr/local/lib/libclusterstack.so"
EOF

# Update Pacemaker system configuration
if [[ $IS_UBUNTU == "Y" ]]; then	# For Debian/Ubuntu
	PACE_MAKER_SYS_CONF="/etc/default/pacemaker";
else								# For RHEL/CentOS
	PACE_MAKER_SYS_CONF="/etc/sysconfig/pacemaker";
fi
sudo tee -a ${PACE_MAKER_SYS_CONF} << EOF
#PCMK_cluster_type=custom
PCMK_stack_file=/usr/local/lib/libclusterstack.so
PCMK_debug=yes
EOF

# Create log directory
sudo mkdir -p /var/log/cluster
#sudo chown hacluster:haclient /var/log/cluster

# Stop existing corosync, Start pacemaker with new stack
echo "Start using ${LIB_NAME}.so";
if true; then
	# Reload systemd configuration
	sudo systemctl daemon-reload;
	sudo systemctl stop pacemaker;

	# Stop corosync if it's running
	sudo systemctl stop corosync;
	sudo systemctl disable corosync;

	sudo systemctl start pacemaker; # Start Pacemaker
	# sudo PCMK_debug=1 /usr/sbin/pacemakerd -f # Run with debug
	sudo systemctl enable pacemaker; # Enable Pacemaker to start on boot
fi

if true; then		# Check if Pacemaker is using our custom stack
	sudo systemctl status pacemaker; sudo pcs status;
	sudo crm status;
	sudo journalctl -u pacemaker -u corosync;
	#tail -f /var/log/cluster/cluster.log
	sudo less -N /var/log/pacemaker/pacemaker.log;
fi
