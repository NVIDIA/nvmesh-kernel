%define _build_id_links none
%global __python /usr/bin/python3
%global __python3 /usr/bin/env python3
%define _python_bytecompile_errors_terminate_build 0

Name:				nvmesh-base
Version:			%{version}
Release:			%{release}
Group:				System Environment
Summary:			"nvmesh-base" by Nvidia

License:			Commercial Non OSI
URL:				http://www.nvidia.com
Source0:			%{name}

Requires:			%{requires_pkgs}
Autoreq:                        0

%description

© Copyright 2025 Nvidia Corporation. All rights reserved. This document contains the confidential and proprietary information of Nvidia Corporation. Do not reproduce or distribute without the prior written consent of Nvidia.

"Nvidia nvmesh-base" includes NVMesh base communication and environment tools.
	Branch: %{branch}
	Commit: %{commit_id}

%prep
cp -rf %{_sourcedir}/%{name} %{_builddir}/

%build

%install
mkdir -pv %{buildroot}/opt/nvmesh/client-repo
mkdir -pv %{buildroot}/opt/nvmesh/public
mkdir -pv %{buildroot}/usr/bin
mkdir -pv %{buildroot}/lib/systemd/system
mkdir -pv %{buildroot}/var/run/nvmesh
mkdir -pv %{buildroot}/etc/nvmesh/keys
mkdir -pv %{buildroot}/etc/nvmesh/nvmesh.conf.d/
mkdir -pv %{buildroot}/var/log/nvmesh
mkdir -pv %{buildroot}/var/opt/nvmesh
mkdir -pv %{buildroot}/opt/nvmesh/bin
mkdir -pv %{buildroot}/opt/nvmesh/common-repo/tools

cp -rf %{_builddir}/%{name}/public/ %{buildroot}/opt/nvmesh
cp -rf %{_builddir}/%{name}/system.d/nvmeshagent.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/system.d/nvmeshcm.service %{buildroot}/lib/systemd/system/
cp -rf %{_builddir}/%{name}/bin/* %{buildroot}/usr/bin/
cp -rf %{_builddir}/%{name}/management_cm %{buildroot}/opt/nvmesh/client-repo
cp -rf %{_builddir}/%{name}/config/nvmesh.conf %{buildroot}/etc/nvmesh/
cp -rf %{_builddir}/%{name}/config/nvmesh.conf.d/* %{buildroot}/etc/nvmesh/nvmesh.conf.d/
cp -rf %{_builddir}/%{name}/dist/pytools/* %{buildroot}/opt/nvmesh/bin

echo "version=\"%{version}-%{release}\"" > %{buildroot}/opt/nvmesh/client-repo/base-version
echo "commit=\"%{commit_id}\"" >> %{buildroot}/opt/nvmesh/client-repo/base-version
echo "branch=\"%{branch}\"" >> %{buildroot}/opt/nvmesh/client-repo/base-version

echo "/opt/nvmesh
/var/run/nvmesh
/var/log/nvmesh
/var/opt/nvmesh
/etc/nvmesh
/usr/bin/nvmesh_configure_management_server
/usr/bin/nvmesh_configure_nics
/lib/systemd/system/nvmeshcm.service
/lib/systemd/system/nvmeshagent.service
/etc/nvmesh/keys
/etc/nvmesh/nvmesh.conf.d/00-readme.conf
/etc/nvmesh/nvmesh.conf" > files.lst

%post
exit 0

%preun
exit 0

%files -f files.lst

%config(noreplace) /etc/nvmesh/nvmesh.conf

%changelog
* Tue Mar 5 2024 Nvidia
- Installing Nvidia nvmesh-base
