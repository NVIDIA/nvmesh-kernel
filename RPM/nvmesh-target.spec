%define _build_id_links none
%global __python /usr/bin/python3
%global __python3 /usr/bin/env python3

#The above command tells the RPM build process to drop the build-id links for gdb debug (since we already package it in the client RPM, otherwise it causes conflicts on install)
Name:				nvmesh-target
Version:			%{version}
Release:			%{release}
Group:				System Environment/Kernel
Summary:			"nvmesh-target" by Nvidia

License:			Commercial Non OSI
URL:				http://www.nvidia.com
Source0:			%{name}

Requires:			%{requires_pkgs}
Autoreq:                        0

%description

© Copyright 2025 Nvidia Corporation. All rights reserved. This document contains the confidential and proprietary information of Nvidia Corporation. Do not reproduce or distribute without the prior written consent of Nvidia.

"Nvidia nvmesh-target" includes NVMesh target components.
	Branch: %{branch}
	Block Size: %{block_size}
	Commit: %{commit_id}
	Kernel: %{kern_ver}
	OFED: %{ofed_ver}
	TOMA UDP: %{toma_udp}


%prep
cp -rf %{_sourcedir}/%{name} %{_builddir}/

%build

%install
mkdir -pv %{buildroot}/opt/nvmesh/target-repo/installation-scripts-%{version}-%{release}
mkdir -pv %{buildroot}/opt/nvmesh/target-repo/services
mkdir -pv %{buildroot}/lib/systemd/system
mkdir -pv %{buildroot}/usr/bin
mkdir -pv %{buildroot}/var/run/nvmesh/nvmeshtarget
mkdir -pv %{buildroot}/var/opt/nvmesh/toma
mkdir -pv %{buildroot}/etc/nvmesh
mkdir -pv %{buildroot}/var/opt/nvmesh/mcs/TOMA
mkdir -pv %{buildroot}/var/log/nvmesh
mkdir -pv %{buildroot}/opt/nvmesh/common-repo/toma.d/
mkdir -pv %{buildroot}/opt/nvmesh/common-repo/tools/

cp -rf %{_builddir}/%{name}/target_* %{buildroot}/opt/nvmesh/target-repo
cp -rf %{_builddir}/%{name}/uninstall-target %{buildroot}/opt/nvmesh/target-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/install.py %{buildroot}/opt/nvmesh/target-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/post_install %{buildroot}/opt/nvmesh/target-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/init.d/nvmeshtarget %{buildroot}/opt/nvmesh/target-repo/services/
cp -rf %{_builddir}/%{name}/system.d/nvmeshtarget.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/system.d/nvmeshtoma.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/bin_target/* %{buildroot}/usr/bin/
cp -rf %{_builddir}/%{name}/scripts/target %{buildroot}/opt/nvmesh/target-repo/scripts
cp -rf %{_builddir}/%{name}/upgrade_scripts/nvmesh-target %{buildroot}/opt/nvmesh/target-repo
mv %{buildroot}/opt/nvmesh/target-repo/nvmesh-target %{buildroot}/opt/nvmesh/target-repo/upgrade_scripts
mv -n %{buildroot}/opt/nvmesh/target-repo/target_*/tools/toma_rpc %{buildroot}/opt/nvmesh/common-repo/tools/
mv -n %{buildroot}/opt/nvmesh/target-repo/target_*/tools/toma_link %{buildroot}/opt/nvmesh/common-repo/tools/
cp -rf %{_builddir}/%{name}/toma/toma.d/* %{buildroot}/opt/nvmesh/common-repo/toma.d/
cp -rf %{_builddir}/%{name}/toma/toma_trace.config %{buildroot}/var/log/nvmesh/
cp -rnf %{buildroot}/opt/nvmesh/target-repo/*/toma/bin/*/nvmeibt_toma_src_tar.pgp %{buildroot}/var/log/nvmesh/ #-n stands to prevent errors on more than once copies
cp -rf %{_builddir}/%{name}/config/target_devices.conf %{buildroot}/etc/nvmesh/
echo "version=\"%{version}-%{release}\"" > %{buildroot}/opt/nvmesh/target-repo/version
echo "commit=\"%{commit_id}\"" >> %{buildroot}/opt/nvmesh/target-repo/version
echo "branch=\"%{branch}\"" >> %{buildroot}/opt/nvmesh/target-repo/version
touch %{buildroot}/var/opt/nvmesh/.target_devices
touch %{buildroot}/var/opt/nvmesh/target_upgrade_version
touch %{buildroot}/opt/nvmesh/common-repo/tools/toma_rpc.config
cp -rf %{_builddir}/%{name}/examples/auto_takeover_drives_spec_example %{buildroot}/var/opt/nvmesh

ln -s /opt/nvmesh/common-repo/tools/toma_link %{buildroot}/opt/nvmesh/common-repo/tools/gpt_util


echo "/opt/nvmesh
/var/run/nvmesh
/var/log/nvmesh
/var/opt/nvmesh
/usr/bin/nvmesh_target
/lib/systemd/system/nvmeshtarget.service
/lib/systemd/system/nvmeshtoma.service
/etc/nvmesh/target_devices.conf
/var/log/nvmesh/toma_trace.config
/var/log/nvmesh/nvmeibt_toma_src_tar.pgp
%ghost /opt/nvmesh/common-repo/tools/toma_rpc.config
%ghost /var/opt/nvmesh/.target_devices
%ghost /var/opt/nvmesh/target_upgrade_version
/var/opt/nvmesh/auto_takeover_drives_spec_example" > files.lst

%pre
# an upgrade
if [ "$1" = "2" ] || [ "$1" = "upgrade" ]; then
	MDIR="/opt/nvmesh/target-repo"
	compressed_kos=`find -L $MDIR -name '*.ko.xz*' -type f 2>/dev/null`
	# if compressed kos found then remove old decompressed kos
	if [ -d "$MDIR" ] && [ ! -z "$compressed_kos" ]; then
		find -L $MDIR -name '*.ko' -type f -exec rm -f {} +
	fi
fi

%post
/opt/nvmesh/target-repo/installation-scripts-%{version}-%{release}/post_install "$1" "$2" "%{version}" "%{release}" "target"
# this should either move to the infra post install phase or the rpm creation phase (since we now work with xz.bcp which is not recognized by modinfo)
# modinfo $(find -L /opt/nvmesh/target-repo/ -name 'nvmeibs.ko' -o -name 'nvmeibs.ko.xz' -type f) -F nvmeibs_capabilities > /opt/nvmesh/target-repo/.capabilities


%preun
/opt/nvmesh/target-repo/installation-scripts-%{version}-%{release}/uninstall-target $1

%postun
/sbin/depmod -a > /dev/null 2>&1

%files -f files.lst

%config(noreplace) /etc/nvmesh/target_devices.conf
%config(noreplace) /var/log/nvmesh/toma_trace.config

%changelog
* Tue Mar 5 2024 Nvidia
- Installing Nvidia nvmesh-target
