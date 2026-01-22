Name:				nvmesh-exlog
Version:			%{version}
Release:			%{release}
Group:				System Environment/Kernel
Summary:			"nvmesh-exlog" by NVIDIA

License:			GPL-2.0-only OR Apache-2.0 at your choice
URL:				http://www.nvidia.com
Source0:			%{name}

Requires:			%{requires_pkgs}

%description

Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. This document contains the confidential and proprietary information of Nvidia Corporation. Do not reproduce or distribute without the prior written consent of Nvidia.

"Excelero nvmesh-exlog" component.
	Branch: %{branch}
	Commit: %{commit_id}
	Kernel: %{kern_ver}
	OFED: %{ofed_ver}

%prep
cp -rf %{_sourcedir}/%{name} %{_builddir}/

%build

%install
mkdir -pv %{buildroot}/usr/bin
cp -rf %{_builddir}/%{name}/bin_exlog/* %{buildroot}/usr/bin/
mkdir -pv %{buildroot}/opt/nvmesh/exlog-repo/installation-scripts-%{version}-%{release}
cp -rf %{_builddir}/%{name}/uninstall-exlog %{buildroot}/opt/nvmesh/exlog-repo/installation-scripts-%{version}-%{release}

%preun
/opt/nvmesh/exlog-repo/installation-scripts-%{version}-%{release}/uninstall-exlog $1

%files
/usr/bin/exlog
/opt/nvmesh

%changelog
* Tue Nov 7 2017 Nvidia
- Installing Nvidia  nvmesh-exlog
