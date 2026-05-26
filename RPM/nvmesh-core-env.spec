Name:				nvmesh-core
Version:			%{version}
Release:			%{release}
Group:				System Environment
Summary:			"nvmesh-core" by NVIDIA

License:			GPL-2.0-only OR Apache-2.0 at your choice
URL:				http://www.nvidia.com
Source0:			%{name}

Requires:			%{requires_pkgs}
Autoreq:                        0

%description

Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

"Nvidia nvmesh-core" includes NVMesh client and target services and components for a kmod-nvmesh-core environment.
	Branch: %{branch}
	Block Size: %{block_size}
	Commit: %{commit_id}
	Kernel: %{kern_ver}
	OFED: %{ofed_ver}


%prep
cp -rf %{_sourcedir}/%{name} %{_builddir}/

%build

%install
mkdir -pv %{buildroot}/opt/nvmesh/client-repo/installation-scripts-%{version}-%{release}
mkdir -pv %{buildroot}/opt/nvmesh/target-repo/installation-scripts-%{version}-%{release}
mkdir -pv %{buildroot}/opt/nvmesh/client-repo/services
mkdir -pv %{buildroot}/opt/nvmesh/target-repo/services
mkdir -pv %{buildroot}/opt/nvmesh/common-repo
mkdir -pv %{buildroot}/lib/systemd/system
mkdir -pv %{buildroot}/usr/bin
mkdir -pv %{buildroot}/etc/modprobe.d
mkdir -pv %{buildroot}/etc/depmod.d
mkdir -pv %{buildroot}/etc/udev/rules.d
mkdir -pv %{buildroot}/var/run/nvmesh/nvmeshclient
mkdir -pv %{buildroot}/var/run/nvmesh/nvmeshtarget
mkdir -pv %{buildroot}/var/opt/nvmesh/toma
mkdir -pv %{buildroot}/etc/opt/NVMesh
mkdir -pv %{buildroot}/var/opt/nvmesh/block_devices_configuration
mkdir -pv %{buildroot}/var/opt/nvmesh/clnt_instance_configuration
mkdir -pv %{buildroot}/var/opt/nvmesh/mcs/CLIENT
mkdir -pv %{buildroot}/var/opt/nvmesh/mcs/TOMA
mkdir -pv %{buildroot}/var/log/nvmesh/trace_daemon
mkdir -pv %{buildroot}/var/log/NVMesh

cp -rf %{_builddir}/%{name}/toma %{buildroot}/opt/nvmesh/target-repo
cp -rf %{_builddir}/%{name}/uninstall-target %{buildroot}/opt/nvmesh/target-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/install.py %{buildroot}/opt/nvmesh/target-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/init.d/nvmeshtarget %{buildroot}/opt/nvmesh/target-repo/services/
cp -rf %{_builddir}/%{name}/system.d/nvmeshtarget.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/system.d/nvmeshtoma.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/system.d/nvmeshagent.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/system.d/nvmeshcm.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/system.d/nvmeshtrace@.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/bin_target/* %{buildroot}/usr/bin/
cp -rf %{_builddir}/%{name}/rules.d/* %{buildroot}/etc/udev/rules.d/
cp -rf %{_builddir}/%{name}/sysctl %{buildroot}/opt/nvmesh/
cp -rf %{_builddir}/%{name}/scripts/target %{buildroot}/opt/nvmesh/target-repo/scripts
cp -rf %{_builddir}/%{name}/scripts/client %{buildroot}/opt/nvmesh/client-repo/scripts
cp -rf %{_builddir}/%{name}/scripts/common %{buildroot}/opt/nvmesh/common-repo/scripts
cp -rf %{_builddir}/%{name}/upgrade_scripts/NVMesh-target %{buildroot}/opt/nvmesh/target-repo
mv %{buildroot}/opt/nvmesh/target-repo/NVMesh-target %{buildroot}/opt/nvmesh/target-repo/upgrade_scripts
cp -rf %{_builddir}/%{name}/uninstall-client %{buildroot}/opt/nvmesh/client-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/install.py %{buildroot}/opt/nvmesh/client-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/post_install %{buildroot}/opt/nvmesh/client-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/tools %{buildroot}/opt/nvmesh/common-repo/
cp -rf %{_builddir}/%{name}/bin_client/* %{buildroot}/usr/bin/
cp -rf %{_builddir}/%{name}/init.d/nvmeshclient %{buildroot}/opt/nvmesh/client-repo/services/
cp -rf %{_builddir}/%{name}/init.d/nvmesh_util %{buildroot}/opt/nvmesh/client-repo/services/
cp -rf %{_builddir}/%{name}/system.d/nvmeshclient.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/modprobe.d/nvmesh.conf %{buildroot}/etc/modprobe.d/
cp -rf %{_builddir}/%{name}/depmod.d/zz02-nvmesh.conf %{buildroot}/etc/depmod.d/
cp -rf %{_builddir}/%{name}/upgrade_scripts/NVMesh-client %{buildroot}/opt/nvmesh/client-repo
mv %{buildroot}/opt/nvmesh/client-repo/NVMesh-client %{buildroot}/opt/nvmesh/client-repo/upgrade_scripts
cp -rf %{_builddir}/%{name}/management_cm %{buildroot}/opt/nvmesh/client-repo
cp -rf %{_builddir}/%{name}/config/nvmesh.conf %{buildroot}/etc/opt/nvmesh/
cp -rf %{_builddir}/%{name}/config/target_devices.conf %{buildroot}/etc/opt/nvmesh/
echo "version=\"%{version}-%{release}\"" > %{buildroot}/opt/nvmesh/client-repo/version
echo "commit=\"%{commit_id}\"" >> %{buildroot}/opt/nvmesh/client-repo/version
echo "branch=\"%{branch}\"" >> %{buildroot}/opt/nvmesh/client-repo/version
cp %{buildroot}/opt/nvmesh/client-repo/version %{buildroot}/opt/nvmesh/target-repo/
touch %{buildroot}/var/opt/nvmesh/.target_devices
touch %{buildroot}/var/opt/nvmesh/client_upgrade_version
touch %{buildroot}/var/opt/nvmesh/target_upgrade_version

ln -s /opt/NVMesh %{buildroot}/opt/nvmesh
ln -s /etc/opt/NVMesh %{buildroot}/etc/nvmesh
ln -s /var/log/NVMesh %{buildroot}/var/log/nvmesh
ln -s /var/opt/NVMesh %{buildroot}/var/opt/nvmesh
ln -s /var/run/NVMesh %{buildroot}/var/run/nvmesh
ln -s /opt/nvmesh/common-repo/tools/traces_post_processor/pager %{buildroot}/var/log/nvmesh/trace_daemon/pager
ln -s /opt/nvmesh/common-repo/tools/traces_post_processor/pager.py %{buildroot}/var/log/nvmesh/trace_daemon/pager.py
ln -s /opt/nvmesh/target-repo/toma/scripts/gpt_util.sh %{buildroot}/opt/nvmesh/common-repo/tools/gpt_util

if [ -d %{buildroot}/opt/nvmesh/client-repo/management_cm/exeServices ]; then
	ln -s /opt/nvmesh/client-repo/management_cm/exeServices/managementAgent %{buildroot}/opt/nvmesh/client-repo/management_cm/managementAgent.py
	ln -s /opt/nvmesh/client-repo/management_cm/exeServices/managementCM %{buildroot}/opt/nvmesh/client-repo/management_cm/managementCM.py
fi

echo "/opt/NVMesh
/var/run/NVMesh
/var/log/NVMesh
/var/opt/NVMesh
/var/opt/nvmesh
/var/log/nvmesh
/var/run/nvmesh
/etc/nvmesh
/opt/nvmesh
/usr/bin/nvmesh_configure_management_server
/usr/bin/nvmesh_configure_nics
/usr/bin/nvmesh_client_instance_do
/usr/bin/nvmesh_clnt_shutdown
/usr/bin/nvmesh_target
/lib/systemd/system/nvmeshclient.service
/lib/systemd/system/nvmeshtarget.service
/lib/systemd/system/nvmeshtoma.service
/lib/systemd/system/nvmeshcm.service
/lib/systemd/system/nvmeshagent.service
/lib/systemd/system/nvmeshtrace@.service
/etc/modprobe.d/nvmesh.conf
/etc/depmod.d/zz02-nvmesh.conf
/etc/opt/nvmesh/nvmesh.conf
/etc/opt/nvmesh/target_devices.conf
/etc/udev/rules.d/60-nvmesh.rules
%ghost /var/opt/nvmesh/.target_devices
%ghost /var/opt/nvmesh/client_upgrade_version
%ghost /var/opt/nvmesh/target_upgrade_version" > files.lst

if [ -e /usr/lib/python2.7 ]; then
	mkdir -pv %{buildroot}/usr/lib/python2.7/dist-packages
	python2 -O -m compileall %{buildroot}/usr/lib/python2.7/dist-packages/*
	python2 -m compileall %{buildroot}/usr/lib/python2.7/dist-packages/*

	for file in $(find %{buildroot}/usr/lib/python2.7/dist-packages/* -type f -printf "%f\n");
	do
		echo "/usr/lib/python2.7/dist-packages/$file" >> files.lst
	done
fi

%post
/opt/nvmesh/client-repo/installation-scripts-%{version}-%{release}/post_install "$1" "$2" "%{version}" "%{release}"

%preun
#this order is critical - target must be first
/opt/nvmesh/target-repo/installation-scripts-%{version}-%{release}/uninstall-target $1
if [ "$?" -ne 0 ]; then
	exit 1
fi

/opt/nvmesh/client-repo/installation-scripts-%{version}-%{release}/uninstall-client $1
if [ "$?" -ne 0 ]; then
        exit 1
fi

%files -f files.lst

%config(noreplace) /etc/opt/nvmesh/nvmesh.conf
%config(noreplace) /etc/opt/nvmesh/target_devices.conf

%changelog
* Wed Oct 7 2015 nvmesh
- Installing NVIDIA nvmesh-core for a kmod-nvmesh-core environment
