Name:				nvmesh-core
Version:			%{version}
Release:			%{release}
Group:				System Environment
Summary:			"nvmesh-core" by Excelero

License:			Commercial Non OSI
URL:				http://www.excelero.com
Source0:			%{name}

Requires:			%{requires_pkgs}
Autoreq:                        0

%description

© Copyright 2015-2020 Excelero, Inc. All rights reserved. This document contains the confidential and proprietary information of Excelero, Inc. Do not reproduce or distribute without the prior written consent of Excelero.

"Excelero nvmesh-core" includes NVMesh client and target services and components for a kmod-nvmesh-core environment.
	Branch: %{branch}
	Block Size: %{block_size}
	Commit: %{commit_id}
	Kernel: %{kern_ver}
	OFED: %{ofed_ver}


%prep
cp -rf %{_sourcedir}/%{name} %{_builddir}/

%build

%install
mkdir -pv %{buildroot}/opt/NVMesh/client-repo/installation-scripts-%{version}-%{release}
mkdir -pv %{buildroot}/opt/NVMesh/target-repo/installation-scripts-%{version}-%{release}
mkdir -pv %{buildroot}/opt/NVMesh/client-repo/services
mkdir -pv %{buildroot}/opt/NVMesh/target-repo/services
mkdir -pv %{buildroot}/opt/NVMesh/common-repo
mkdir -pv %{buildroot}/opt/NVMesh/public
mkdir -pv %{buildroot}/lib/systemd/system
mkdir -pv %{buildroot}/usr/bin
mkdir -pv %{buildroot}/etc/modprobe.d
mkdir -pv %{buildroot}/etc/depmod.d
mkdir -pv %{buildroot}/etc/udev/rules.d
mkdir -pv %{buildroot}/var/run/NVMesh/nvmeshclient
mkdir -pv %{buildroot}/var/run/NVMesh/nvmeshtarget
mkdir -pv %{buildroot}/var/opt/NVMesh/toma
mkdir -pv %{buildroot}/etc/opt/NVMesh
mkdir -pv %{buildroot}/var/opt/NVMesh/block_devices_configuration
mkdir -pv %{buildroot}/var/opt/NVMesh/block_devices_sub_vols
mkdir -pv %{buildroot}/var/opt/NVMesh/clnt_instance_configuration
mkdir -pv %{buildroot}/var/opt/NVMesh/mcs/CLIENT
mkdir -pv %{buildroot}/var/opt/NVMesh/mcs/TOMA
mkdir -pv %{buildroot}/var/log/NVMesh/trace_daemon
mkdir -pv %{buildroot}/var/log/NVMesh

cp -rf %{_builddir}/%{name}/public/ %{buildroot}/opt/NVMesh
cp -rf %{_builddir}/%{name}/toma %{buildroot}/opt/NVMesh/target-repo
cp -rf %{_builddir}/%{name}/uninstall-target %{buildroot}/opt/NVMesh/target-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/install.py %{buildroot}/opt/NVMesh/target-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/init.d/nvmeshtarget %{buildroot}/opt/NVMesh/target-repo/services/
cp -rf %{_builddir}/%{name}/system.d/nvmeshtarget.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/system.d/nvmeshtoma.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/system.d/nvmeshagent.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/system.d/nvmeshcm.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/system.d/nvmeshtrace@.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/bin_target/* %{buildroot}/usr/bin/
cp -rf %{_builddir}/%{name}/rules.d/* %{buildroot}/etc/udev/rules.d/
cp -rf %{_builddir}/%{name}/sysctl %{buildroot}/opt/NVMesh/
cp -rf %{_builddir}/%{name}/scripts/target %{buildroot}/opt/NVMesh/target-repo/scripts
cp -rf %{_builddir}/%{name}/scripts/client %{buildroot}/opt/NVMesh/client-repo/scripts
cp -rf %{_builddir}/%{name}/scripts/common %{buildroot}/opt/NVMesh/common-repo/scripts
cp -rf %{_builddir}/%{name}/upgrade_scripts/NVMesh-target %{buildroot}/opt/NVMesh/target-repo
cp -rf %{_builddir}/%{name}/mlnx_fw/Excelero_mlxconfig.db %{buildroot}/etc/opt/NVMesh
cp -rf %{_builddir}/%{name}/mlnx_fw/patch_mlxconfig.db.sql %{buildroot}/etc/opt/NVMesh
mv %{buildroot}/opt/NVMesh/target-repo/NVMesh-target %{buildroot}/opt/NVMesh/target-repo/upgrade_scripts
cp -rf %{_builddir}/%{name}/uninstall-client %{buildroot}/opt/NVMesh/client-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/install.py %{buildroot}/opt/NVMesh/client-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/post_install %{buildroot}/opt/NVMesh/client-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/tools %{buildroot}/opt/NVMesh/common-repo/
cp -rf %{_builddir}/%{name}/bin_client/* %{buildroot}/usr/bin/
cp -rf %{_builddir}/%{name}/init.d/nvmeshclient %{buildroot}/opt/NVMesh/client-repo/services/
cp -rf %{_builddir}/%{name}/init.d/nvmesh_util %{buildroot}/opt/NVMesh/client-repo/services/
cp -rf %{_builddir}/%{name}/system.d/nvmeshclient.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/modprobe.d/nvmesh.conf %{buildroot}/etc/modprobe.d/
cp -rf %{_builddir}/%{name}/depmod.d/zz02-nvmesh.conf %{buildroot}/etc/depmod.d/
cp -rf %{_builddir}/%{name}/upgrade_scripts/NVMesh-client %{buildroot}/opt/NVMesh/client-repo
mv %{buildroot}/opt/NVMesh/client-repo/NVMesh-client %{buildroot}/opt/NVMesh/client-repo/upgrade_scripts
cp -rf %{_builddir}/%{name}/management_cm %{buildroot}/opt/NVMesh/client-repo
cp -rf %{_builddir}/%{name}/config/nvmesh.conf %{buildroot}/etc/opt/NVMesh/
cp -rf %{_builddir}/%{name}/config/target_devices.conf %{buildroot}/etc/opt/NVMesh/
echo "version=\"%{version}-%{release}\"" > %{buildroot}/opt/NVMesh/client-repo/version
echo "commit=\"%{commit_id}\"" >> %{buildroot}/opt/NVMesh/client-repo/version
echo "branch=\"%{branch}\"" >> %{buildroot}/opt/NVMesh/client-repo/version
cp %{buildroot}/opt/NVMesh/client-repo/version %{buildroot}/opt/NVMesh/target-repo/
touch %{buildroot}/var/opt/NVMesh/.target_devices
touch %{buildroot}/var/opt/NVMesh/client_upgrade_version
touch %{buildroot}/var/opt/NVMesh/target_upgrade_version

ln -s /opt/NVMesh %{buildroot}/opt/nvmesh
ln -s /etc/opt/NVMesh %{buildroot}/etc/nvmesh
ln -s /var/log/NVMesh %{buildroot}/var/log/nvmesh
ln -s /var/opt/NVMesh %{buildroot}/var/opt/nvmesh
ln -s /var/run/NVMesh %{buildroot}/var/run/nvmesh
ln -s /opt/NVMesh/common-repo/tools/traces_post_processor/pager %{buildroot}/var/log/NVMesh/trace_daemon/pager
ln -s /opt/NVMesh/common-repo/tools/traces_post_processor/pager.py %{buildroot}/var/log/NVMesh/trace_daemon/pager.py
ln -s /opt/NVMesh/common-repo/tools/toma_link %{buildroot}/opt/NVMesh/common-repo/tools/gpt_util

if [ -d %{buildroot}/opt/NVMesh/client-repo/management_cm/exeServices ]; then
	ln -s /opt/NVMesh/client-repo/management_cm/exeServices/managementAgent %{buildroot}/opt/NVMesh/client-repo/management_cm/managementAgent.py
	ln -s /opt/NVMesh/client-repo/management_cm/exeServices/managementCM %{buildroot}/opt/NVMesh/client-repo/management_cm/managementCM.py
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
/etc/opt/NVMesh/nvmesh.conf
/etc/opt/NVMesh/target_devices.conf
/etc/opt/NVMesh/Excelero_mlxconfig.db
/etc/opt/NVMesh/patch_mlxconfig.db.sql
/etc/udev/rules.d/60-nvmesh.rules
%ghost /var/opt/NVMesh/.target_devices
%ghost /var/opt/NVMesh/client_upgrade_version
%ghost /var/opt/NVMesh/target_upgrade_version" > files.lst

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
/opt/NVMesh/client-repo/installation-scripts-%{version}-%{release}/post_install "$1" "$2" "%{version}" "%{release}"

%preun
#this order is critical - target must be first
/opt/NVMesh/target-repo/installation-scripts-%{version}-%{release}/uninstall-target $1
if [ "$?" -ne 0 ]; then
	exit 1
fi

/opt/NVMesh/client-repo/installation-scripts-%{version}-%{release}/uninstall-client $1
if [ "$?" -ne 0 ]; then
        exit 1
fi

%files -f files.lst

%config(noreplace) /etc/opt/NVMesh/nvmesh.conf
%config(noreplace) /etc/opt/NVMesh/target_devices.conf

%changelog
* Wed Oct 7 2015 Excelero
- Installing Excelero nvmesh-core for a kmod-nvmesh-core environment
