Name:				nvmesh-cli
Version:			%{version}
Release:			%{release}
Group:				System Environment/Kernel
Summary:			"nvmesh-cli" by NVIDIA

License:			GPL-2.0-only OR Apache-2.0 at your choice
URL:				http://www.nvidia.com
Source0:			%{name}

%description

Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

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
