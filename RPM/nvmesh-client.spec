%global __python /usr/bin/python3
%global __python3 /usr/bin/env python3

Name:				nvmesh-client
Version:			%{version}
Release:			%{release}
Group:				System Environment/Kernel
Summary:			"nvmesh-client" by Nvidia

License:			Commercial Non OSI
URL:				http://www.nvidia.com
Source0:			%{name}

Requires:			%{requires_pkgs}
Autoreq:                        0

%description

© Copyright 2025 Nvidia Corporation. All rights reserved. This document contains the confidential and proprietary information of Nvidia Corporation. Do not reproduce or distribute without the prior written consent of Nvidia.

"Nvidia nvmesh-client" includes NVMesh client and common components.
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
mkdir -pv %{buildroot}/opt/nvmesh/client-repo/services
mkdir -pv %{buildroot}/opt/nvmesh/common-repo
mkdir -pv %{buildroot}/opt/nvmesh/common-repo/modprobe.d
mkdir -pv %{buildroot}/lib/systemd/system
mkdir -pv %{buildroot}/usr/bin
mkdir -pv %{buildroot}/etc/modprobe.d
mkdir -pv %{buildroot}/etc/depmod.d
mkdir -pv %{buildroot}/etc/udev/rules.d
mkdir -pv %{buildroot}/etc/nvmesh
mkdir -pv %{buildroot}/var/run/nvmesh/nvmeshclient
mkdir -pv %{buildroot}/var/opt/nvmesh
mkdir -pv %{buildroot}/var/opt/nvmesh/block_devices_configuration
mkdir -pv %{buildroot}/var/opt/nvmesh/block_devices_sub_vols
mkdir -pv %{buildroot}/var/opt/nvmesh/clnt_instance_configuration
mkdir -pv %{buildroot}/var/opt/nvmesh/mcs/CLIENT
mkdir -pv %{buildroot}/var/log/nvmesh/trace_daemon
mkdir -pv %{buildroot}/var/log/nvmesh/trace_daemon/.cache
mkdir -pv %{buildroot}/var/log/nvmesh

cp -rf %{_builddir}/%{name}/system.d/nvmeshtrace@.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/rules.d/* %{buildroot}/etc/udev/rules.d/
cp -rf %{_builddir}/%{name}/sysctl %{buildroot}/opt/nvmesh/
cp -rf %{_builddir}/%{name}/scripts/client %{buildroot}/opt/nvmesh/client-repo/scripts
cp     %{_builddir}/%{name}/clnt/block/datapath_utils_generic/profiling/nvmesh_profiling.py %{buildroot}/opt/nvmesh/client-repo/scripts/
cp -rf %{_builddir}/%{name}/scripts/common %{buildroot}/opt/nvmesh/common-repo/scripts
cp -rf %{_builddir}/%{name}/mlnx_fw/Excelero_mlxconfig.db %{buildroot}/etc/nvmesh
cp -rf %{_builddir}/%{name}/mlnx_fw/patch_mlxconfig.db.sql %{buildroot}/etc/nvmesh
cp -rf %{_builddir}/%{name}/client_* %{buildroot}/opt/nvmesh/client-repo
cp -rf %{_builddir}/%{name}/uninstall-client %{buildroot}/opt/nvmesh/client-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/install.py %{buildroot}/opt/nvmesh/client-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/post_install %{buildroot}/opt/nvmesh/client-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/common_* %{buildroot}/opt/nvmesh/common-repo
cp -rf %{_builddir}/%{name}/tools %{buildroot}/opt/nvmesh/common-repo
mv %{buildroot}/opt/nvmesh/common-repo/common_*/tools/traces_post_processor/cpager %{buildroot}/opt/nvmesh/common-repo/tools/traces_post_processor/
mv -n %{buildroot}/opt/nvmesh/common-repo/common_*/tools/infra_shared/infra_shared.so %{buildroot}/opt/nvmesh/common-repo/tools/
mv -n %{buildroot}/opt/nvmesh/common-repo/common_*/tools/nvmesh_netlink.py %{buildroot}/opt/nvmesh/common-repo/tools/
mv -n %{buildroot}/opt/nvmesh/common-repo/common_*/tools/read_dwarf.py %{buildroot}/opt/nvmesh/common-repo/tools/
mv -n %{buildroot}/opt/nvmesh/common-repo/common_*/tools/the-calculator.py %{buildroot}/opt/nvmesh/common-repo/tools/
mv -n %{buildroot}/opt/nvmesh/common-repo/common_*/tools/nvmesh_memmgr_monitor.py %{buildroot}/opt/nvmesh/common-repo/tools/
mv -n %{buildroot}/opt/nvmesh/common-repo/common_*/tools/nvmesh_metrics.py %{buildroot}/opt/nvmesh/common-repo/tools/
mv -n %{buildroot}/opt/nvmesh/common-repo/common_*/tools/nvmesh_client_upgrade_breakdown.py %{buildroot}/opt/nvmesh/common-repo/tools/
cp -rf %{_builddir}/%{name}/bin_client/* %{buildroot}/usr/bin/
#backward compatible
mv -n %{buildroot}/usr/bin/nvmesh_clnt_shutdown.py %{buildroot}/usr/bin/nvmesh_clnt_shutdown

cp -rf %{_builddir}/%{name}/init.d/nvmeshclient %{buildroot}/opt/nvmesh/client-repo/services/
cp -rf %{_builddir}/%{name}/init.d/nvmesh_util %{buildroot}/opt/nvmesh/client-repo/services/
cp -rf %{_builddir}/%{name}/system.d/nvmeshclient.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/modprobe.d/nvmesh.conf %{buildroot}/etc/modprobe.d/
cp -rf %{_builddir}/%{name}/modprobe.d/* %{buildroot}/opt/nvmesh/common-repo/modprobe.d/
cp -rf %{_builddir}/%{name}/depmod.d/zz02-nvmesh.conf %{buildroot}/etc/depmod.d/
cp -rf %{_builddir}/%{name}/upgrade_scripts/nvmesh-client %{buildroot}/opt/nvmesh/client-repo
mv %{buildroot}/opt/nvmesh/client-repo/nvmesh-client %{buildroot}/opt/nvmesh/client-repo/upgrade_scripts
echo "version=\"%{version}-%{release}\"" > %{buildroot}/opt/nvmesh/client-repo/version
echo "commit=\"%{commit_id}\"" >> %{buildroot}/opt/nvmesh/client-repo/version
echo "branch=\"%{branch}\"" >> %{buildroot}/opt/nvmesh/client-repo/version
touch %{buildroot}/var/opt/nvmesh/client_upgrade_version

ln -s /opt/nvmesh/bin/pager %{buildroot}/var/log/nvmesh/trace_daemon/pager
ln -s /opt/nvmesh/common-repo/tools/traces_post_processor/pager.py %{buildroot}/var/log/nvmesh/trace_daemon/pager.py
ln -s /opt/nvmesh/common-repo/tools/traces_post_processor/cpager %{buildroot}/var/log/nvmesh/trace_daemon/cpager

echo "/opt/nvmesh
%attr(0444, root, root) /opt/nvmesh/common-repo/modprobe.d
%attr(0444, root, root) /opt/nvmesh/common-repo/modprobe.d/*
/var/run/nvmesh
/var/log/nvmesh
/var/opt/nvmesh
/usr/bin/nvmesh_client_instance_do
/usr/bin/nvmesh_clnt_shutdown
/usr/bin/nvmesh_update
/lib/systemd/system/nvmeshclient.service
/lib/systemd/system/nvmeshtrace@.service
/etc/modprobe.d/nvmesh.conf
/etc/depmod.d/zz02-nvmesh.conf
/etc/nvmesh/Excelero_mlxconfig.db
/etc/nvmesh/patch_mlxconfig.db.sql
/etc/udev/rules.d/60-nvmesh.rules
%ghost /var/opt/nvmesh/client_upgrade_version" > files.lst

%pre
# an upgrade
if [ "$1" = "2" ] || [ "$1" = "upgrade" ]; then
	MDIRS="/opt/nvmesh/client-repo /opt/nvmesh/common-repo"
	compressed_kos=`find -L $MDIRS -name '*.ko.xz*' -type f 2>/dev/null`
	# if compressed kos found then remove old decompressed kos
	if [ ! -z "$compressed_kos" ]; then
		find -L $MDIRS -name '*.ko' -type f -exec rm -f {} +
	fi
fi

%post
/opt/nvmesh/client-repo/installation-scripts-%{version}-%{release}/post_install "$1" "$2" "%{version}" "%{release}" "client"
# this should either move to the infra post install phase or the rpm creation phase (since we now work with xz.bcp which is not recognized by modinfo)
# modinfo $(find -L /opt/nvmesh/client-repo/ -name 'nvmeibc.ko' -o -name 'nvmeibc.ko.xz' -type f) -F nvmeibc_capabilities > /opt/nvmesh/client-repo/.capabilities

%preun
/opt/nvmesh/client-repo/installation-scripts-%{version}-%{release}/uninstall-client $1

%postun
/sbin/depmod -a > /dev/null 2>&1

%files -f files.lst
%dir %attr(777, root, root) /var/log/nvmesh/trace_daemon/.cache

%changelog
* Tue Mar 5 2024 Nvidia
- Installing Nvidia nvmesh-client
