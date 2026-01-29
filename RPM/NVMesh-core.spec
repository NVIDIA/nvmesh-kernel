Name:               nvmesh-core
Version:            %{version}
Release:            %{release}
Group:              System Environment/Kernel
Summary:            "nvmesh-core" by NVIDIA

License:            GPL-2.0-only OR Apache-2.0 at your choice
URL:                http://www.nvidia.com
Source0:            %{name}

Requires(post):           %{requires_pkgs}
Autoreq:                        0

%description

Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

transitional package from nvmesh-core to nvmesh-base, nvmesh-client and nvmesh-target.
This is a transitional package of NVMesh-core. It can safely be removed.

"Nvidia NVMesh-core" component.
	Branch: %{branch}
	Commit: %{commit_id}

%prep
cp -rf %{_sourcedir}/%{name} %{_builddir}/

%build

%pre
unlink /lib/modules/$(uname -r)/extra/nvmesh/client 2> /dev/null
unlink /lib/modules/$(uname -r)/extra/nvmesh/common 2> /dev/null
unlink /lib/modules/$(uname -r)/extra/nvmesh/target 2> /dev/null
exit 0

%install

%post
isUpgrade=$1
client_upgrade_file=/var/opt/NVMesh/client_upgrade_version
target_upgrade_file=/var/opt/NVMesh/target_upgrade_version

if grep -i Ubuntu /etc/*release > /dev/null 2>&1; then
	if [ "$1" == "configure" ] && [ -z "$2" ]; then
	        isUpgrade=1
	elif [ "$1" == "abort-upgrade" ] || [ "$1" == "abort-remove" ]; then
		exit 0
	else
		isUpgrade=2
		oldVersionRelease=$2
	fi
else
	oldVersionRelease=`rpm -q --queryformat "%{version}-%{release}|" nvmesh-core | cut -d '|' -f1`
fi

if [ $isUpgrade -eq 2 ]; then
	if [ ! -e $client_upgrade_file ]; then
		echo "$oldVersionRelease" > $client_upgrade_file
	fi

	if [ ! -e $target_upgrade_file ]; then
		echo "$oldVersionRelease" > $target_upgrade_file
	fi
fi

#copy cache files to new location
cp /var/opt/NVMesh/target_upgrade_version /var/opt/nvmesh/target_upgrade_version 2>/dev/null
cp /var/opt/NVMesh/client_upgrade_version /var/opt/nvmesh/client_upgrade_version 2>/dev/null
cp /var/opt/NVMesh/client-upgrade.err /var/opt/nvmesh/client-upgrade.err 2>/dev/null
cp /var/opt/NVMesh/target-upgrade.err /var/opt/nvmesh/target-upgrade.err 2>/dev/null
cp /opt/NVMesh/common-repo/tools/toma_rpc.config /opt/nvmesh/common-repo/tools/toma_rpc.config 2>/dev/null
cp /var/opt/NVMesh/.target_devices /var/opt/nvmesh/.target_devices 2>/dev/null
cp -r /var/opt/NVMesh/toma/* /var/opt/nvmesh/toma/ 2>/dev/null
cp -r /var/opt/NVMesh/block_devices_configuration/* /var/opt/nvmesh/block_devices_configuration/ 2>/dev/null
cp -r /var/opt/NVMesh/block_devices_sub_vols/* /var/opt/nvmesh/block_devices_sub_vols/ 2>/dev/null
cp -r /var/opt/NVMesh/clnt_instance_configuration/* /var/opt/nvmesh/clnt_instance_configuration 2>/dev/null
cp -r /var/opt/NVMesh/mcs/* /var/opt/nvmesh/mcs/ 2>/dev/null
cp -r /etc/opt/NVMesh/keys/* /etc/nvmesh/keys/ 2>/dev/null

exit 0

%preun
#remove cache from old location
rm  /var/opt/NVMesh/target_upgrade_version 2>/dev/null
rm /var/opt/NVMesh/client_upgrade_version 2>/dev/null
rm /var/opt/NVMesh/client-upgrade.err 2>/dev/null
rm /var/opt/NVMesh/target-upgrade.err 2>/dev/null
rm /opt/NVMesh/common-repo/tools/toma_rpc.config 2>/dev/null
rm /var/opt/NVMesh/.target_devices 2>/dev/null
rm -r /var/opt/NVMesh/toma/* 2>/dev/null
rm -r /var/opt/NVMesh/block_devices_configuration/* 2>/dev/null
rm -r /var/opt/NVMesh/block_devices_sub_vols/* 2>/dev/null
rm -r /var/opt/NVMesh/clnt_instance_configuration/* 2>/dev/null
rm -r /var/opt/NVMesh/mcs/CLIENT/* 2>/dev/null
rm -r /var/opt/NVMesh/mcs/TOMA/* 2>/dev/null
rm -r /etc/opt/NVMesh/keys/* 2>/dev/null
exit 0
#check uninstall files (client/target)

%files

%changelog
* Tue Mar 5 2024 Nvidia
- Installing Nvidia nvmesh-core transitional package
