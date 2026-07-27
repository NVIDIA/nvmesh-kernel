Name:				nvmesh-cli
Version:			%{version}
Release:			%{release}
Group:				System Environment/Kernel
Summary:			"nvmesh-cli" by Excelero

License:			Commercial Non OSI
URL:				http://www.nvidia.com
Source0:			%{name}

%description

© Copyright 2025 Nvidia Corporation. All rights reserved. This document contains the confidential and proprietary information of Nvidia Corporation. Do not reproduce or distribute without the prior written consent of Nvidia.

"Nvidia nvmesh-cli" components.
	Branch: %{branch}
	Commit: %{commit_id}

%prep
cp -rf %{_sourcedir}/%{name} %{_builddir}/

%build

%install
mkdir -pv %{buildroot}/opt/nvmesh/cli

echo "version=\"%{version}-%{release}\"" > %{buildroot}/opt/nvmesh/cli/version
echo "commit=\"%{commit_id}\"" >> %{buildroot}/opt/nvmesh/cli/version
echo "branch=\"%{branch}\"" >> %{buildroot}/opt/nvmesh/cli/version

%post
echo ""

%postun
MDIR="/opt/nvmesh"
if [ -d "$MDIR" ] && [ -z "$(ls -A $MDIR)" ]; then
	rm -rf $MDIR
else
	exit 0
fi

%files
/opt/nvmesh/cli

%changelog
* Wed Oct 7 2015 Nvidia
- Installing Nvidia nvmesh-cli
